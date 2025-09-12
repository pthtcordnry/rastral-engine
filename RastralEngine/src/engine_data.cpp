#include "renderer.h"
#include "music_director.h"

struct EngineData {
	audio_engine g_audio;
	MusicDirector g_md;
	bool          g_audioReady = false;
	float         g_rage = 0.0f;
	bool          g_vsyncOn = true;
};

struct RenderState {
	RenderTarget gRT_Scene = {};
	GLuint gProgramMesh = 0;
	GLuint gProgramPost = 0;

	GLuint gVAO_Mesh = 0;
	GLuint gVBO_Mesh = 0;
	GLuint gEBO_Mesh = 0;
	GLuint gVAO_Post = 0;
	GLuint gVBO_Post = 0;

	float gUserScale = 1.0f;
	float gCamDist = 5.0f;
	float gYaw = 0.0f;
	float gPitch = 0.0f;
	bool  gWireframe = false;
};