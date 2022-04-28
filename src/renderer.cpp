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

void GTR::Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

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

	//Codigo para ordenar los rendercalls
	std::sort(render_calls.begin(), render_calls.end(), [](RenderCall rc1, RenderCall rc2) {
		if(rc1.material->alpha_mode == GTR::eAlphaMode::BLEND && rc2.material->alpha_mode == GTR::eAlphaMode::BLEND) rc1.distance_to_camera > rc2.distance_to_camera;
		return rc1.distance_to_camera < rc2.distance_to_camera;
	});

	for (int i = 0; i < lights.size(); i++) {
		if (lights[i]->cast_shadows) generateShadowMap(lights[i]);
	}

	for (int i = 0; i < render_calls.size(); i++) {
		if (camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderMeshWithMaterial(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, camera);
	}

	glViewport(Application::instance->window_width-256, 0, 256, 256);
	showShadowMap(lights[0]);
	glViewport(0, 0, Application::instance->window_width, Application::instance->window_height);
}

void GTR::Renderer::showShadowMap(LightEntity* light) {
	if (!light->shadowmap) return;
	Shader* shader = Shader::getDefaultShader("depth");
	shader->enable();
	shader->setUniform("u_camera_nearfar", Vector2(light->light_camera->near_plane, light->light_camera->far_plane));
	light->fbo->depth_texture->toViewport(shader);
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
void GTR::Renderer::renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	Texture* texture = NULL;
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
	if (!scene->multipass)	shader = Shader::Get("singlepass");
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
		shader->setUniform("u_texture", texture, 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
	shader->setUniform("u_ambient_light", scene->ambient_light);

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
		if (!scene->multipass) {
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
			float light_max_distance[5];
			int light_type[5];
			for (int i = 0; i < lights.size(); i++) {
				light_position[i] = lights[i]->model * Vector3();
				light_color[i] = lights[i]->color * lights[i]->intensity;
				light_max_distance[i] = lights[i]->max_distance;
				light_front[i] = lights[i]->model.rotateVector(Vector3(0, 0, -1));
				light_cone[i] = Vector3(lights[i]->cone_angle, lights[i]->cone_exp, cos(lights[i]->cone_angle * DEG2RAD));
				if (lights[i]->light_type == GTR::eLightType::DIRECTIONAL) light_type[i] = 0; //No funciona la direccional en el singlepass, preguntar agenjo
				else if (lights[i]->light_type == GTR::eLightType::SPOT) light_type[i] = 1;
				else light_type[i] = 2; //Preguntar por el area size
			}
			shader->setUniform3Array("u_light_position", (float*)&light_position, 4);
			shader->setUniform3Array("u_light_color", (float*)&light_color, 4);
			shader->setUniform3Array("u_light_front", (float*)&light_front, 4);
			shader->setUniform3Array("u_light_cone", (float*)&light_cone, 4);
			shader->setUniform1Array("u_light_max_distance", (float*)&light_max_distance, 4);
			shader->setUniform1Array("u_light_type", (int*)&light_type, 4);
			shader->setUniform1("u_num_lights", 4);
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

				if (light->light_type == GTR::eLightType::DIRECTIONAL) shader->setUniform("u_light_type", 0);
				else if (light->light_type == GTR::eLightType::SPOT) shader->setUniform("u_light_type", 1);
				else shader->setUniform("u_light_type", 2);

				if (light->shadowmap) {
					shader->setUniform("u_light_cast_shadows", 1);
					shader->setUniform("u_light_shadowmap", light->shadowmap, 1);
					shader->setUniform("u_light_shadowmap_vp", light->light_camera->viewprojection_matrix);
					shader->setUniform("u_light_shadow_bias", light->shadow_bias);
				}
				else {
					shader->setUniform("u_light_cast_shadows", 0);
				}

				//do the draw call that renders the mesh into the screen
				mesh->render(GL_TRIANGLES);


				shader->setUniform("u_ambient_light", Vector3()); //Solo queremos pintar 1 vez la luz ambiente
			}
		}
	}
	
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

//Generates a ShadowMap for the given light
void GTR::Renderer::generateShadowMap(LightEntity* light) {
	if (light->light_type != GTR::eLightType::SPOT) return;

	if (!light->cast_shadows) { //No estas haciendo un control de errores extra?
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

	glClear(GL_DEPTH_BUFFER_BIT);

	Camera* current_camera = Camera::current;
	Camera* light_camera = light->light_camera;
	light_camera->setPerspective(light->cone_angle, 1.0, 0.1, light->max_distance);
	light_camera->lookAt(light->model.getTranslation(), light->model * Vector3(0, 0, -1), light->model.rotateVector(Vector3(0, 1, 0)));
	light_camera->enable();

	for (int i = 0; i < render_calls.size(); i++) {
		if (render_calls[i].material->alpha_mode == GTR::eAlphaMode::BLEND) continue;
		if (light_camera->testBoxInFrustum(render_calls[i].world_bounding.center, render_calls[i].world_bounding.halfsize))
			renderShadowMap(render_calls[i].model, render_calls[i].mesh, render_calls[i].material, light_camera);
	}

	light->fbo->unbind();

	current_camera->enable();
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
	else
		glEnable(GL_CULL_FACE);
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