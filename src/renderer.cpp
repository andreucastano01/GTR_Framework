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
	pipeline = FORWARD;
	light_render = SINGLEPASS;
	gbuffers_fbo = NULL;
}

void GTR::Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	lights.clear();
	render_calls.clear();

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
			lights.push_back(lent);
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

	/*glViewport(Application::instance->window_width - 256, 0, 256, 256);
	showShadowMap(lights[0]);
	glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);*/
}

void GTR::Renderer::renderForward(GTR::Scene* scene, Camera* camera) {
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < render_calls.size(); i++) {
		if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderMeshWithMaterialandLight(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}
}

void GTR::Renderer::renderDeferred(GTR::Scene* scene, Camera* camera) {
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	if (!gbuffers_fbo) {
		//create and FBO
		gbuffers_fbo = new FBO();

		//create 3 textures of 4 components
		gbuffers_fbo->create(width, height,	3, GL_RGBA, GL_UNSIGNED_BYTE, true);
	}

	gbuffers_fbo->bind();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	for (int i = 0; i < render_calls.size(); i++) {
		if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderMeshWithMaterialtoGBuffer(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}

	gbuffers_fbo->unbind();

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

	glViewport(0, 0, width, height);

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
				shader->setUniform("u_light_color", light->color * light->intensity);
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