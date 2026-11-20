/*
 *  gl_shader_util.h - Minimal GLSL helper for gfxaccel OpenGL backend
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef GFXACCEL_GL_SHADER_UTIL_H
#define GFXACCEL_GL_SHADER_UTIL_H

#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>

static inline GLuint gfx_gl_compile_shader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	if (!sh) return 0;
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		GLsizei len = 0;
		glGetShaderInfoLog(sh, sizeof(log), &len, log);
		fprintf(stderr, "[gfxaccel-gl] shader compile failed: %.*s\n", (int)len, log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static inline GLuint gfx_gl_link_program(GLuint vs, GLuint fs)
{
	GLuint prog = glCreateProgram();
	if (!prog) return 0;
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];
		GLsizei len = 0;
		glGetProgramInfoLog(prog, sizeof(log), &len, log);
		fprintf(stderr, "[gfxaccel-gl] program link failed: %.*s\n", (int)len, log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static inline GLuint gfx_gl_make_program(const char *vs_src, const char *fs_src)
{
	GLuint vs = gfx_gl_compile_shader(GL_VERTEX_SHADER, vs_src);
	GLuint fs = gfx_gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}
	GLuint prog = gfx_gl_link_program(vs, fs);
	glDeleteShader(vs);
	glDeleteShader(fs);
	return prog;
}

#endif /* GFXACCEL_GL_SHADER_UTIL_H */
