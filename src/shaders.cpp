#include "shaders.hpp"
#include <csignal>
#include <string>

#include <GLES2/gl2.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Shader.hpp>

#include "shader_content.hpp"

Hy3Shaders::Hy3Shaders() {
	{
		auto& s = this->tab;
		s.shader = makeShared<CShader>();
		s.shader->createProgram(std::string(SHADER_TAB_VERT), std::string(SHADER_TAB_FRAG));
		GLuint program = s.shader->program();
		s.posAttrib = glGetAttribLocation(program, "pos");
		s.proj = glGetUniformLocation(program, "proj");
		s.monitorSize = glGetUniformLocation(program, "monitorSize");
		s.pixelOffset = glGetUniformLocation(program, "pixelOffset");
		s.pixelSize = glGetUniformLocation(program, "pixelSize");
		s.applyBlur = glGetUniformLocation(program, "applyBlur");
		s.blurTex = glGetUniformLocation(program, "blurTex");
		s.opacity = glGetUniformLocation(program, "opacity");
		s.fillColor = glGetUniformLocation(program, "fillColor");
		s.borderColor = glGetUniformLocation(program, "borderColor");
		s.borderWidth = glGetUniformLocation(program, "borderWidth");
		s.outerRadius = glGetUniformLocation(program, "outerRadius");
	}
}

Hy3Shaders::~Hy3Shaders() {
	// CShader destructor handles cleanup via destroy()
}

Hy3Shaders* Hy3Shaders::instance() {
	static auto* INSTANCE = new Hy3Shaders();
	return INSTANCE;
}