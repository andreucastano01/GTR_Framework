#include "renderer.h"

#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "fbo.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "application.h"
#include "extra/hdre.h"
#include <algorithm>


using namespace GTR;

GTR::Renderer::Renderer() {
	direct_light = NULL;
	pipeline = DEFERRED;
	light_render = MULTIPASS;
	irradiance_mode = NORMAL;
	gbuffers_fbo = NULL;
	illumination_fbo = NULL;
	ssao_fbo = NULL;
	decals_fbo = NULL;
	show_gbuffers = false;
	show_ssao = false;
	ssaoplus = false;
	show_irr_texture = false;
	ssao_blur = NULL;
	irr_fbo = NULL;
	probes_texture = NULL;
	postFX_textureA = NULL;
	postFX_textureB = NULL;
	postFX_textureC = NULL;
	postFX_textureD = NULL;
	blurred_texture = NULL;
	average_lum = 1.0;
	lum_white = 1.0;
	lum_scale = 1.0;

	vigneting = 1.0;
	saturation = 1.0;
	debug_factor = 1.0;
	debug_factor2 = 1.0;
	threshold = 0.9;
	contrast = 1.0;


	skybox = CubemapFromHDRE("data/night.hdre");
	cube.createCube();

	ssao_random_points = generateSpherePoints(128, 1, false);
	ssaoplus_random_points = generateSpherePoints(128, 1, true);
}

void GTR::Renderer::generateSkybox(Camera* camera) {
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false, false);
	Shader* shader = Shader::Get("skybox");
	shader->enable();

	Matrix44 model;

	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	model.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	model.scale(5, 5, 5);

	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform("u_texture", skybox, 0);

	mesh->render(GL_TRIANGLES);
	shader->disable();

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
}

void GTR::Renderer::generateProbes(GTR::Scene* scene) {
	probes.clear();
	//define the corners of the axis aligned grid
	//this can be done using the boundings of our scene
	Vector3 start_pos(-300, 5, -300);
	Vector3 end_pos(300, 150, 300);

	//define how many probes you want per dimension
	Vector3 dim(12, 6, 12);

	startpos = start_pos;
	endpos = end_pos;
	dimpos = dim;

	//compute the vector from one corner to the other
	Vector3 delta = (end_pos - start_pos);

	//and scale it down according to the subdivisions
	//we substract one to be sure the last probe is at end pos
	delta.x /= (dim.x - 1);
	delta.y /= (dim.y - 1);
	delta.z /= (dim.z - 1);

	for (int z = 0; z < dim.z; ++z) {
		for (int y = 0; y < dim.y; ++y) {
			for (int x = 0; x < dim.x; ++x)
			{
				sProbe p;
				p.local.set(x, y, z);

				//index in the linear array
				p.index = x + y * dim.x + z * dim.x * dim.y;

				//and its position
				p.pos = start_pos + delta * Vector3(x, y, z);
				probes.push_back(p);
			}
		}
	}

	for (int iP = 0; iP < probes.size(); iP++)
	{
		std::cout << "Generando probe numero " << iP << " de " << probes.size() << std::endl;
		int probe_index = iP;
		captureProbe(probes[iP], scene);
	}

	if (probes_texture != NULL) delete probes_texture;

	//create the texture to store the probes (do this ONCE!!!)
	probes_texture = new Texture(
		9, //9 coefficients per probe
		probes.size(), //as many rows as probes
		GL_RGB, //3 channels per coefficient
		GL_FLOAT); //they require a high range

		//we must create the color information for the texture. because every SH are 27 floats in the RGB,RGB,... order, we can create an array of SphericalHarmonics and use it as pixels of the texture
	SphericalHarmonics* sh_data = NULL;
	sh_data = new SphericalHarmonics[dim.x * dim.y * dim.z];

	//here we fill the data of the array with our probes in x,y,z order
	for (int i = 0; i < probes.size(); ++i)
		sh_data[i] = probes[i].sh;

	//now upload the data to the GPU as a texture
	probes_texture->upload(GL_RGB, GL_FLOAT, false, (uint8*)sh_data);

	//disable any texture filtering when reading
	probes_texture->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	//always free memory after allocating it!!!
	delete[] sh_data;

	probes_texture->unbind();
}

void GTR::Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	lights.clear();
	render_calls.clear();
	decals.clear();

	//render entities and lights
	for (int i = 0; i < scene->entities.size(); ++i)
	{
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab) 
				renderPrefab(ent->model, pent->prefab, camera);
		}

		//is a light!
		if (ent->entity_type == LIGHT) {
			LightEntity* lent = (GTR::LightEntity*)ent;
			if (lent->light_type == GTR::eLightType::DIRECTIONAL) direct_light = lent;
			lights.push_back(lent);
		}

		//is a decal!
		if (ent->entity_type == DECALL) {
			DecalEntity* dent = (GTR::DecalEntity*)ent;
			decals.push_back(dent);
		}
	}

	//Ordenar rendercalls
	std::sort(render_calls.begin(), render_calls.end(), [](RenderCall rc1, RenderCall rc2) {
		if(rc1.material->alpha_mode == GTR::eAlphaMode::BLEND && rc2.material->alpha_mode == GTR::eAlphaMode::BLEND) rc1.distance_to_camera > rc2.distance_to_camera;
		return rc1.distance_to_camera < rc2.distance_to_camera;
	});

	for (int i = 0; i < lights.size(); i++) {
		if (lights[i]->cast_shadows) generateShadowMap(lights[i]);
	}

	if (pipeline == FORWARD) renderForward(scene, camera);
	else renderDeferred(scene, camera);

	if (probes_texture && show_irr_texture) probes_texture->toViewport();
}

//Aqui hay algo que no me genera bien la luz
void GTR::Renderer::renderForward(GTR::Scene* scene, Camera* camera) {
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();
	generateSkybox(camera);

	for (int i = 0; i < render_calls.size(); i++) {
		if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderMeshWithMaterialandLight(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}

	for(int i = 0; i < probes.size(); i++)
		renderProbe(probes[i].pos, 2, probes[i].sh.coeffs[0].v);

	/*glViewport(Application::instance->window_width - 256, 0, 256, 256);
	showShadowMap(lights[0]); //Showing shadowmap from light 0, if need change light number
	glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);*/
}


void GTR::Renderer::renderDeferred(GTR::Scene* scene, Camera* camera){
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;
	Shader* shader = NULL;

	if (!gbuffers_fbo) {
		//create and FBO
		gbuffers_fbo = new FBO();

		//create 3 textures of 4 components
		gbuffers_fbo->create(width, height,	3, GL_RGBA, GL_UNSIGNED_BYTE, true);

		decals_fbo = new FBO();

		decals_fbo->create(width, height, 3, GL_RGBA, GL_UNSIGNED_BYTE, true);
	}

	Mesh* quad = Mesh::getQuad();
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	gbuffers_fbo->bind();
	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < render_calls.size(); i++) {
		if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderMeshWithMaterialtoGBuffer(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}

	gbuffers_fbo->unbind();

	//decals
	gbuffers_fbo->color_textures[0]->copyTo(decals_fbo->color_textures[0]);
	gbuffers_fbo->color_textures[1]->copyTo(decals_fbo->color_textures[1]);
	gbuffers_fbo->color_textures[2]->copyTo(decals_fbo->color_textures[2]);
	
	decals_fbo->bind();
	gbuffers_fbo->depth_texture->copyTo(NULL);
	decals_fbo->unbind();

	if (decals.size()) {
		gbuffers_fbo->bind();

		shader = Shader::Get("decal");
		shader->enable();
		shader->setUniform("u_depth_texture", decals_fbo->depth_texture, 4);
		shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		shader->setUniform("u_inverse_viewprojection", inv_vp);
		shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColorMask(true, true, true, false);

		for (int i = 0; i < decals.size(); i++) {
			DecalEntity* decal = decals[i];
			Texture* decal_texture = Texture::Get(decal->texture.c_str());
			if (!decal_texture) continue;
			shader->setUniform("u_decal_texture", decal_texture, 5);
			shader->setUniform("u_model", decal->model);
			
			Matrix44 imodel = decal->model;
			imodel.inverse();
			shader->setUniform("u_imodel", imodel);
			cube.render(GL_TRIANGLES);
		}

		glColorMask(true, true, true, true);
		glDisable(GL_BLEND);
		gbuffers_fbo->unbind();
	}

	if (!ssao_fbo) {
		//create and FBO
		ssao_fbo = new FBO();

		//create 1 texture of 3 components
		ssao_fbo->create(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
	}

	ssao_fbo->bind();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	if (ssaoplus) {
		shader = Shader::Get("ssaoplus");
		shader->enable();
		shader->setUniform3Array("u_points", (float*)&ssaoplus_random_points[0], ssaoplus_random_points.size());
	}
	else {
		shader = Shader::Get("ssao");
		shader->enable();
		shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 2);
		shader->setUniform3Array("u_points", (float*)&ssao_random_points[0], ssao_random_points.size());
	}
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 4);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));

	quad->render(GL_TRIANGLES);

	ssao_fbo->unbind();

	if (!ssao_blur) {
		ssao_blur = new FBO();
		ssao_blur->create(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
	}

	ssao_blur->bind();

	shader = Shader::Get("ssao_blur");
	shader->enable();
	shader->setUniform("ssaoInput", ssao_fbo->color_textures[0], 0);

	quad->render(GL_TRIANGLES);

	ssao_blur->unbind();

	if (!illumination_fbo) {
		//create and FBO
		illumination_fbo = new FBO();

		//create 1 texture of 3 components
		illumination_fbo->create(width, height, 1, GL_RGB, GL_FLOAT, true);

		postFX_textureA = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureB = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureC = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		postFX_textureD = new Texture(width, height, GL_RGB, GL_FLOAT, false);
		blurred_texture = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}

	illumination_fbo->bind();

	gbuffers_fbo->depth_texture->copyTo(NULL);
	glClear(GL_COLOR_BUFFER_BIT);

	glDisable(GL_DEPTH_TEST);
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	checkGLErrors();
	generateSkybox(camera);

	shader = Shader::Get("deferred");
	shader->enable();
	gbuffertoshader(gbuffers_fbo, scene, camera, shader);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	
	shader->setUniform("u_ssao_texture", ssao_blur->color_textures[0], 5);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_camera_position", camera->eye);

	lightToShader(direct_light, shader);

	if (probes_texture) {
		shader->setUniform("u_irr", 1.0f);
		shader->setUniform("u_irr_texture", probes_texture, 6);
		shader->setUniform("u_irr_start", startpos);
		shader->setUniform("u_irr_end", endpos);
		shader->setUniform("u_irr_dim", dimpos);
		shader->setUniform("u_irr_normal_distance", 0.1f);
		shader->setUniform("u_num_probes", probes_texture->height);
		shader->setUniform("u_irr_delta", endpos - startpos);
	}
	else shader->setUniform("u_irr", 0.0f);
	
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	quad->render(GL_TRIANGLES);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glFrontFace(GL_CW);
	glEnable(GL_CULL_FACE);

	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false, false);
	shader = Shader::Get("sphere_deferred");
	shader->enable();

	for (int i = 0; i < lights.size(); i++) {
		LightEntity* light = lights[i];
		if (light->light_type == GTR::eLightType::SPOT || light->light_type == GTR::eLightType::POINT) {
			gbuffertoshader(gbuffers_fbo, scene, camera, shader);
			shader->setUniform("u_inverse_viewprojection", inv_vp);
			shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
			shader->setUniform("u_ambient_light", Vector3()); //Solo queremos pintar 1 vez la luz ambiente
			lightToShader(light, shader);
			Matrix44 m;
			vec3 position = light->model * Vector3();
			m.setTranslation(position.x, position.y, position.z);
			//and scale it according to the max_distance of the light
			m.scale(light->max_distance, light->max_distance, light->max_distance);
			shader->setUniform("u_model", m);
			shader->setUniform("u_camera_position", camera->eye);
			//do the draw call that renders the mesh into the screen
			sphere->render(GL_TRIANGLES);
		}
	}

	glFrontFace(GL_CCW);
	glDisable(GL_CULL_FACE);

	//Render alpha nodes
	glEnable(GL_DEPTH_TEST);
	for (int i = 0; i < render_calls.size(); i++) {
		if (render_calls[i].material->alpha_mode == eAlphaMode::BLEND)
			if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
				renderMeshWithMaterialandLight(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}


	illumination_fbo->unbind();

	applyFX(illumination_fbo->color_textures[0], gbuffers_fbo->depth_texture, camera);

	if (show_ssao) {
		glDisable(GL_BLEND);
		ssao_fbo->color_textures[0]->toViewport();
	}
	if (show_gbuffers) {
		glDisable(GL_BLEND);
		glViewport(0, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[0]->toViewport();

		glViewport(width * 0.5, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[1]->toViewport();

		glViewport(0, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[2]->toViewport();

		glViewport(width * 0.5, 0, width * 0.5, height * 0.5);
		Shader* shader = Shader::getDefaultShader("depth");
		shader->enable();
		shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));
		gbuffers_fbo->depth_texture->toViewport(shader);
		shader->disable();

		glViewport(0, 0, width, height);
	}
}

void GTR::Renderer::applyFX(Texture* color_texture, Texture* depth_texture, Camera* camera) {
	Shader* shader = NULL;
	Texture* current_texture = color_texture;
	FBO* fbo = NULL;
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	//Blur
	for (int i = 0; i < 16; i++) {
		fbo = Texture::getGlobalFBO(postFX_textureA);
		fbo->bind();
		shader = Shader::Get("blur");
		shader->enable();
		shader->setUniform("u_offset", vec2(pow(1.0f, i) / current_texture->width, 0.0) * debug_factor);
		shader->setUniform("u_intensity", 1.0f);
		current_texture->toViewport(shader);
		fbo->unbind();

		fbo = Texture::getGlobalFBO(blurred_texture);
		fbo->bind();
		shader = Shader::Get("blur");
		shader->enable();
		shader->setUniform("u_offset", vec2(0.0, pow(1.0f, i) / current_texture->height) * debug_factor);
		shader->setUniform("u_intensity", 1.0f);
		postFX_textureA->toViewport(shader);
		fbo->unbind();
	}

	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("dof");
	shader->enable();
	shader->setUniform("u_textureB", blurred_texture, 1);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_depth_texture", depth_texture, 2);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Chromatic aberration + lens distorsion
	/*fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("chrlns");
	shader->enable();
	shader->setUniform("resolution", Vector2((float)Application::instance->window_width, (float)Application::instance->window_width));
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);*/

	//Motion Blur
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("motionblur");
	shader->enable();
	shader->setUniform("u_depth_texture", depth_texture, 1);
	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_viewprojection_old", vp_matrix_last);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	vp_matrix_last = camera->viewprojection_matrix;


	//Saturation + Vigneting
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("vigneting");
	shader->enable();
	shader->setUniform("u_vigneting", vigneting);
	shader->setUniform("u_saturation", saturation);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//FFXA (acabar esto)
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("ffxa");
	shader->enable();
	shader->setUniform("u_viewportSize", Vector2((float)Application::instance->window_width, (float)Application::instance->window_width));
	shader->setUniform("u_iViewportSize", Vector2(1.0 / (float)Application::instance->window_width, 1.0 / (float)Application::instance->window_height));
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);

	//Bloom
	fbo = Texture::getGlobalFBO(postFX_textureC);
	fbo->bind();
	shader = Shader::Get("contrast");
	shader->enable();
	shader->setUniform("u_intensity", contrast);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureC;

	//LUT
	/*fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("lut");
	shader->enable();
	shader->setUniform("u_amount", 3.0f);
	shader->setUniform("u_textureB", postFX_textureC, 1);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);*/


	fbo = Texture::getGlobalFBO(postFX_textureD);
	fbo->bind();
	shader = Shader::Get("threshold");
	shader->enable();
	shader->setUniform("u_threshold", threshold);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureD;

	for (int i = 0; i < 16; i++) {
		fbo = Texture::getGlobalFBO(postFX_textureA);
		fbo->bind();
		shader = Shader::Get("blur");
		shader->enable();
		shader->setUniform("u_offset", vec2(pow(1.0f, i) / current_texture->width, 0.0) * debug_factor);
		shader->setUniform("u_intensity", 1.0f);
		current_texture->toViewport(shader);
		fbo->unbind();

		fbo = Texture::getGlobalFBO(postFX_textureB);
		fbo->bind();
		shader = Shader::Get("blur");
		shader->enable();
		shader->setUniform("u_offset", vec2(0.0, pow(1.0f,i) / current_texture->height) * debug_factor);
		shader->setUniform("u_intensity", 1.0f);
		postFX_textureA->toViewport(shader);
		fbo->unbind();
		current_texture = postFX_textureB;
	}
	
	fbo = Texture::getGlobalFBO(postFX_textureA);
	fbo->bind();
	shader = Shader::Get("mix");
	shader->enable();
	shader->setUniform("u_intensity", debug_factor2);
	shader->setUniform("u_textureB", postFX_textureC, 1);
	current_texture->toViewport(shader);
	fbo->unbind();
	current_texture = postFX_textureA;
	std::swap(postFX_textureA, postFX_textureB);


	//Tonemapper
	shader = Shader::Get("tonemapper");
	shader->enable();
	shader->setUniform("u_average_lum", average_lum);
	shader->setUniform("u_lumwhite2", lum_white * lum_white);
	shader->setUniform("u_scale", lum_scale);

	glDisable(GL_BLEND);
	current_texture->toViewport(shader);
}

std::vector<Vector3> GTR::generateSpherePoints(int num, float radius, bool hemi) {
	std::vector<Vector3> points;
	points.resize(num);
	for (int i = 0; i < num; i += 1)
	{
		Vector3& p = points[i];
		float u = random();
		float v = random();
		float theta = u * 2.0 * PI;
		float phi = acos(2.0 * v - 1.0);
		float r = cbrt(random() * 0.9 + 0.1) * radius;
		float sinTheta = sin(theta);
		float cosTheta = cos(theta);
		float sinPhi = sin(phi);
		float cosPhi = cos(phi);
		p.x = r * sinPhi * cosTheta;
		p.y = r * sinPhi * sinTheta;
		p.z = r * cosPhi;
		if (hemi && p.z < 0)
			p.z *= -1.0;
	}
	return points;
}


void GTR::Renderer::gbuffertoshader(FBO* gbuffers_fbo, GTR::Scene* scene, Camera* camera, Shader* shader){

	//pass the gbuffers to the shader
	shader->setUniform("u_gb0_texture", gbuffers_fbo->color_textures[0], 1);
	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 2);
	shader->setUniform("u_gb2_texture", gbuffers_fbo->color_textures[2], 3);
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 4);

	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
}

void GTR::Renderer::lightToShader(LightEntity* light, Shader* shader) {
	shader->setUniform("u_light_color", light->color);
	shader->setUniform("u_light_intensity", light->intensity);
	shader->setUniform("u_light_position", light->model * Vector3());
	shader->setUniform("u_light_max_distance", light->max_distance);

	shader->setUniform("u_light_cone", Vector3(light->cone_angle, light->cone_exp, cos(light->cone_angle * DEG2RAD)));
	shader->setUniform("u_light_front", light->model.rotateVector(Vector3(0, 0, -1)));


	if (light->light_type == GTR::eLightType::DIRECTIONAL) {
		shader->setUniform("u_light_type", 0);
		shader->setUniform("u_light_vector", light->model * Vector3() - light->target);
	}
	else if (light->light_type == GTR::eLightType::SPOT) shader->setUniform("u_light_type", 1);
	else shader->setUniform("u_light_type", 2);

	if (light->shadowmap) {
		shader->setUniform("u_light_cast_shadows", light->cast_shadows);
		shader->setUniform("u_light_shadowmap", light->shadowmap, 0);
		shader->setUniform("u_light_shadowmap_vp", light->light_camera->viewprojection_matrix);
		shader->setUniform("u_light_shadow_bias", light->shadow_bias);
	}
	else {
		shader->setUniform("u_light_cast_shadows", 0);
	}
}

void GTR::Renderer::showShadowMap(LightEntity* light) {
	if (!light->shadowmap) return;
	Shader* shader = Shader::getDefaultShader("depth");
	shader->enable();
	shader->setUniform("u_camera_nearfar", Vector2(light->light_camera->near_plane, light->light_camera->far_plane));
	light->fbo->depth_texture->toViewport(shader);
	glEnable(GL_DEPTH_TEST);
}

//Generates a ShadowMap for the given light
void GTR::Renderer::generateShadowMap(LightEntity* light) {
	if (light->light_type == GTR::eLightType::DIRECTIONAL || light->light_type == GTR::eLightType::SPOT) {
		if (!light->cast_shadows) {
			if (light->fbo) {
				delete light->fbo;
				light->fbo = NULL;
				light->shadowmap = NULL;
			}
			return;
		}
		if (!light->fbo) {
			light->fbo = new FBO();
			light->fbo->setDepthOnly(1024, 1024);
			light->shadowmap = light->fbo->depth_texture;
		}
		if (!light->light_camera) light->light_camera = new Camera();

		light->fbo->bind();

		glColorMask(false, false, false, false);

		glClear(GL_DEPTH_BUFFER_BIT);

		Camera* current_camera = Camera::current;
		Camera* light_camera = light->light_camera;

		if (light->light_type == GTR::eLightType::DIRECTIONAL) {
			float halfsize = light->area_size / 2;
			light_camera->setOrthographic(-halfsize, halfsize, -halfsize, halfsize, 0.1, light->max_distance);
			light_camera->lookAt(light->model.getTranslation(), light->model.getTranslation() + light->model.frontVector(), light->model.rotateVector(Vector3(0, 1, 0)));
		}
		if (light->light_type == GTR::eLightType::SPOT) {
			light_camera->setPerspective(light->cone_angle * 2, 1.0, 0.1, light->max_distance);
			light_camera->lookAt(light->model.getTranslation(), light->model * Vector3(0, 0, -1), light->model.rotateVector(Vector3(0, 1, 0)));
		}

		light_camera->enable();
		for (int i = 0; i < render_calls.size(); i++) {
			if (render_calls[i].material->alpha_mode == GTR::eAlphaMode::BLEND) continue;
			if (light_camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
				renderShadowMap(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, light_camera);
		}

		light->fbo->unbind();

		glColorMask(true, true, true, true);

		current_camera->enable();
	}
	else return;
}

//renders all the prefab
void GTR::Renderer::renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	renderNode(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void GTR::Renderer::renderNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model,node->mesh->box);
		
		//render node mesh
		RenderCall rc;
		Vector3 nodepos = node_model.getTranslation();
		rc.mesh = node->mesh;
		rc.material = node->material;
		rc.model = node_model;
		rc.world_bounding = world_bounding;
		rc.distance_to_camera = nodepos.distance(camera->eye);
		if (node->material->alpha_mode == GTR::eAlphaMode::BLEND) rc.distance_to_camera += 1000000;
		render_calls.push_back(rc);

		//renderMeshWithMaterial( node_model, node->mesh, node->material, camera);
		//node->mesh->renderBounding(node_model, true);
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		renderNode(prefab_model, node->children[i], camera);
}

//renders a mesh given its transform and material
void GTR::Renderer::renderMeshWithMaterialandLight(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	Texture* texture = NULL;
	Texture* emissive_texture = NULL;
	Texture* occlusion_texture = NULL;
	Texture* normal_texture = NULL;
	Scene* scene = GTR::Scene::instance;

	int num_lights = lights.size();

	texture = material->color_texture.texture;
	//texture = material->emissive_texture;
	//texture = material->metallic_roughness_texture;
	//texture = material->normal_texture;
	//texture = material->occlusion_texture;
	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

	//select if render both sides of the triangles
	if(material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
    assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	if (light_render == SINGLEPASS)	shader = Shader::Get("singlepass");
	else shader = Shader::Get("multipass");

    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model );
	shader->setUniform("u_camera_position", camera->eye);
	float t = getTime();
	shader->setUniform("u_time", t );

	shader->setUniform("u_color", material->color);
	if(texture)
		shader->setUniform("u_texture", texture, 5);
	
	emissive_texture = material->emissive_texture.texture;
	if (emissive_texture)
		shader->setUniform("u_texture_emissive", emissive_texture, 6);
	
	occlusion_texture = material->metallic_roughness_texture.texture;
	if (occlusion_texture) {
		shader->setUniform("u_texture_occlusion", occlusion_texture, 7);
		shader->setUniform("u_have_occlusion_texture", 1);
	}
	else shader->setUniform("u_have_occlusion_texture", 0);
	
	normal_texture = material->normal_texture.texture;
	if (normal_texture) {
		shader->setUniform("u_texture_normal", normal_texture, 8);
		shader->setUniform("u_have_normal_texture", 1); //Un prefab puede no tener un normal_map
	} else shader->setUniform("u_have_normal_texture", 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform("u_emissive_factor", material->emissive_factor);
	shader->setUniform("u_roughness_factor", material->roughness_factor);
	shader->setUniform("u_metallic_factor", material->metallic_factor);

	glDepthFunc(GL_LEQUAL);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	if (!num_lights) {
		if (material->alpha_mode == GTR::eAlphaMode::BLEND)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		else
			glDisable(GL_BLEND);
		shader->setUniform("u_light_color", Vector3());
		mesh->render(GL_TRIANGLES);
	}
	else {
		if (light_render == SINGLEPASS) {
			//Singlepass
			if (material->alpha_mode == GTR::eAlphaMode::BLEND)
			{
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
			else
				glDisable(GL_BLEND);

			Vector3 light_position[5];
			Vector3 light_color[5];
			Vector3 light_front[5];
			Vector3 light_cone[5];
			Vector3 light_vector[5];
			Matrix44 vp_shadowmap[5];
			int cast_shadows[5];
			float shadow_bias[5];
			float light_max_distance[5];
			int light_type[5];
			for (int i = 0; i < num_lights; i++) {
				light_position[i] = lights[i]->model * Vector3();
				light_color[i] = lights[i]->color * lights[i]->intensity;
				light_max_distance[i] = lights[i]->max_distance;
				light_front[i] = lights[i]->model.rotateVector(Vector3(0, 0, -1));
				light_cone[i] = Vector3(lights[i]->cone_angle, lights[i]->cone_exp, cos(lights[i]->cone_angle * DEG2RAD));
				if (lights[i]->shadowmap) {
					cast_shadows[i] = lights[i]->cast_shadows;
					shader->setUniform("u_light_shadowmap[i]", lights[i]->shadowmap, i);
					vp_shadowmap[i] = lights[i]->light_camera->viewprojection_matrix;
					shadow_bias[i] = lights[i]->shadow_bias;
				}
				else cast_shadows[i] = 0;
				light_vector[i] = lights[i]->model * Vector3() - lights[i]->target;
				if (lights[i]->light_type == GTR::eLightType::DIRECTIONAL) light_type[i] = 0;
				else if (lights[i]->light_type == GTR::eLightType::SPOT) light_type[i] = 1;
				else light_type[i] = 2;
			}
			shader->setMatrix44Array("u_light_shadowmap_vp", (Matrix44*)&vp_shadowmap, num_lights);
			shader->setUniform1Array("u_light_cast_shadows", (int*)&cast_shadows, num_lights);
			shader->setUniform1Array("u_light_shadow_bias", (float*)&shadow_bias, num_lights);
			shader->setUniform3Array("u_light_position", (float*)&light_position, num_lights);
			shader->setUniform3Array("u_light_color", (float*)&light_color, num_lights);
			shader->setUniform3Array("u_light_front", (float*)&light_front, num_lights);
			shader->setUniform3Array("u_light_cone", (float*)&light_cone, num_lights);
			shader->setUniform3Array("u_light_vector", (float*)&light_vector, num_lights);
			shader->setUniform1Array("u_light_max_distance", (float*)&light_max_distance, num_lights);
			shader->setUniform1Array("u_light_type", (int*)&light_type, num_lights);
			shader->setUniform("u_num_lights", num_lights);
			mesh->render(GL_TRIANGLES);		
		}
		//Multipass
		else {
			for (int i = 0; i < lights.size(); i++) {
				if (i == 0) {
					if (material->alpha_mode == GTR::eAlphaMode::BLEND)
					{
						glEnable(GL_BLEND);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					}
					else
						glDisable(GL_BLEND);
				}
				else {
					glBlendFunc(GL_SRC_ALPHA, GL_ONE);
					glEnable(GL_BLEND);
				}
				LightEntity* light = lights[i];
				lightToShader(light, shader);

				//do the draw call that renders the mesh into the screen
				mesh->render(GL_TRIANGLES);

				shader->setUniform("u_ambient_light", Vector3()); //Solo queremos pintar 1 vez la luz ambiente
				shader->setUniform("u_emissive_factor", Vector3()); //Solo queremos pintar 1 vez el factor emisivo
			}
		}
	}
	
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

void GTR::Renderer::renderMeshWithMaterialtoGBuffer(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	if (material->alpha_mode == eAlphaMode::BLEND) return;

	//define locals to simplify coding
	Shader* shader = NULL;
	Texture* texture = NULL;
	Texture* emissive_texture = NULL;
	Texture* occlusion_texture = NULL;
	Texture* normal_texture = NULL;
	Scene* scene = GTR::Scene::instance;

	texture = material->color_texture.texture;
	//texture = material->emissive_texture;
	//texture = material->metallic_roughness_texture;
	//texture = material->normal_texture;
	//texture = material->occlusion_texture;
	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	shader = Shader::Get("gbuffers");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	float t = getTime();
	shader->setUniform("u_time", t);

	shader->setUniform("u_color", material->color);
	if (texture)
		shader->setUniform("u_texture", texture, 5);

	emissive_texture = material->emissive_texture.texture;
	if (emissive_texture)
		shader->setUniform("u_texture_emissive", emissive_texture, 6);

	occlusion_texture = material->metallic_roughness_texture.texture;
	if (occlusion_texture) {
		shader->setUniform("u_texture_occlusion", occlusion_texture, 7);
		shader->setUniform("u_have_occlusion_texture", 1);
	}
	else shader->setUniform("u_have_occlusion_texture", 0);

	normal_texture = material->normal_texture.texture;
	if (normal_texture) {
		shader->setUniform("u_texture_normal", normal_texture, 8);
		shader->setUniform("u_have_normal_texture", 1); //Un prefab puede no tener un normal_map
	}
	else shader->setUniform("u_have_normal_texture", 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
	shader->setUniform("u_emissive_factor", material->emissive_factor);
	shader->setUniform("u_roughness_factor", material->roughness_factor);
	shader->setUniform("u_metallic_factor", material->metallic_factor);

	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();
}

void GTR::Renderer::renderShadowMap(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera) {
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	Scene* scene = GTR::Scene::instance;

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else {
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CW);
	}
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	shader = Shader::Get("flat");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_model", model);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	glFrontFace(GL_CCW);
}

void GTR::Renderer::renderProbe(Vector3 pos, float size, float* coeffs)
{
	Camera* camera = Camera::current;
	Shader* shader = Shader::Get("probe");
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false, false);

	glEnable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	Matrix44 model;
	model.setTranslation(pos.x, pos.y, pos.z);
	model.scale(size, size, size);

	shader->enable();
	shader->setUniform("u_viewprojection",
		camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform3Array("u_coeffs", coeffs, 9);

	mesh->render(GL_TRIANGLES);
}

void GTR::Renderer::captureProbe(sProbe& probe, GTR::Scene* scene) {
	FloatImage images[6]; //here we will store the six views
	Camera cam;

//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	if (irr_fbo == NULL) {
		irr_fbo = new FBO();
		irr_fbo->create(64, 64, 1, GL_RGB, GL_FLOAT);
	}

	for (int i = 0; i < 6; i++) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		Vector3 eye = probe.pos;
		Vector3 front = cubemapFaceNormals[i][2];
		Vector3 center = probe.pos + front;
		Vector3 up = cubemapFaceNormals[i][1];
		cam.lookAt(eye, center, up);
		cam.enable();

		//render the scene from this point of view
		irr_fbo->bind();
		renderForward(scene, &cam);
		irr_fbo->unbind();

		//read the pixels back and store in a FloatImage
		images[i].fromTexture(irr_fbo->color_textures[0]);
	}

	//compute the coefficients given the six images
	probe.sh = computeSH(images);

}


Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = HDRE::Get(filename);
	if (!hdre)
		return NULL;

	Texture* texture = new Texture();
	if (hdre->getFacesf(0))
	{
		texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesf(0),
			hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT);
		for (int i = 1; i < hdre->levels; ++i)
			texture->uploadCubemap(texture->format, texture->type, false,
				(Uint8**)hdre->getFacesf(i), GL_RGBA32F, i);
	}
	else
		if (hdre->getFacesh(0))
		{
			texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesh(0),
				hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_HALF_FLOAT);
			for (int i = 1; i < hdre->levels; ++i)
				texture->uploadCubemap(texture->format, texture->type, false,
					(Uint8**)hdre->getFacesh(i), GL_RGBA16F, i);
		}
	return texture;
}