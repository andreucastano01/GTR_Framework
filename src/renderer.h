#pragma once
#include "prefab.h"

//forward declarations
class Camera;

namespace GTR {
	class Prefab;
	class Material;

	class RenderCall {
	public:
		Mesh* mesh;
		Material* material;
		Matrix44 model;

		BoundingBox world_bounding;
		float distance_to_camera;
	};
	
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code 
	class Renderer
	{
	public:
		enum elightrender {
			SINGLEPASS,
			MULTIPASS
		};

		enum epipeline {
			FORWARD,
			DEFERRED
		};

		//add here your functions
		std::vector<RenderCall> render_calls;
		std::vector<LightEntity*> lights;
		epipeline pipeline;
		elightrender light_render;
		void generateShadowMap(LightEntity* light);
		void renderShadowMap(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void showShadowMap(LightEntity* light);
		bool show_gbuffers;

		FBO* gbuffers_fbo;
		FBO* illumination_fbo;

		Renderer();

		//Render types
		void renderForward(GTR::Scene* scene, Camera* camera);
		void renderDeferred(GTR::Scene* scene, Camera* camera);

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);
	
		//to render a whole prefab (with all its nodes)
		void renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void renderNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterialandLight(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void renderMeshWithMaterialtoGBuffer(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
	};

	Texture* CubemapFromHDRE(const char* filename);
};