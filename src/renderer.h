#pragma once
#include "prefab.h"
#include "sphericalharmonics.h"

//forward declarations
class Camera;
class Shader;

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

	//struct to store probes
	struct sProbe {
		Vector3 pos; //where is located
		Vector3 local; //its ijk pos in the matrix
		int index; //its index in the linear array
		SphericalHarmonics sh; //coeffs
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
		void lightToShader(LightEntity* light, Shader* shader);
		void gbuffertoshader(FBO* gbuffers_fbo, GTR::Scene* scene, Camera* camera, Shader* shader);

		bool show_gbuffers;
		bool show_ssao;
		bool ssaoplus;
		bool show_irr_texture;
		float average_lum;
		float lum_white;
		float lum_scale;

		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		FBO* ssao_fbo;
		FBO* ssao_blur;
		FBO* irr_fbo;
		Texture* probes_texture;

		std::vector<Vector3> ssao_random_points;
		std::vector<Vector3> ssaoplus_random_points;

		std::vector<sProbe> probes;
		Vector3 startpos;
		Vector3 endpos;
		Vector3 dimpos;
		void generateProbes(GTR::Scene* scene);
		void renderProbe(Vector3 pos, float size, float* coeffs);
		void captureProbe(sProbe& probe, GTR::Scene* scene);

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

	std::vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);
};