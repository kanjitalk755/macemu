#include "sysdeps.h"
#include "cpu_emulation.h"
#include "gl_engine.h"
#include "gfxaccelhost.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

uint32_t GuestTickCount(void)
{
	if (!RAMBaseHost)
		return 0;
	return ReadMacInt32(0x016a);
}

void GfxAccelHostBeginFrame(GLContext *ctx)
{
	GLMetalBeginFrame(ctx);
}

void GfxAccelHostBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLMetalBitmap(ctx, width, height, bgra_data, data_len);
}

void GfxAccelHostClear(GLContext *ctx, uint32_t mask)
{
	GLMetalClear(ctx, mask);
}

void GfxAccelHostDestroyTexture(GLTextureObject *texObj)
{
	GLMetalDestroyTexture(texObj);
}

void GfxAccelHostDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len)
{
	GLMetalDrawPixels(ctx, width, height, bgra_data, data_len);
}

void GfxAccelHostEndFrame(GLContext *ctx)
{
	GLMetalEndFrame(ctx);
}

void GfxAccelHostFlushImmediateMode(GLContext *ctx)
{
	GLMetalFlushImmediateMode(ctx);
}

void GfxAccelHostInit(GLContext *ctx)
{
	GLMetalInit(ctx);
}

uint8_t * GfxAccelHostReadFramebufferRect(GLContext *ctx, int x, int y, int width, int height, int *out_len)
{
	return GLMetalReadFramebufferRect(ctx, x, y, width, height, out_len);
}

void GfxAccelHostRelease(GLContext *ctx)
{
	GLMetalRelease(ctx);
}

void GfxAccelHostUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, int depth, const uint8_t *data, int dataLen)
{
	GLMetalUpload3DTexture(ctx, texObj, level, width, height, depth, data, dataLen);
}

void GfxAccelHostUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int w, int h, const uint8_t *data, int bytesPerRow)
{
	GLMetalUploadSubTexture(ctx, texObj, level, xoff, yoff, w, h, data, bytesPerRow);
}

void GfxAccelHostUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int zoff, int w, int h, int d, const uint8_t *data, int bytesPerRow, int bytesPerImage)
{
	GLMetalUploadSubTexture3D(ctx, texObj, level, xoff, yoff, zoff, w, h, d, data, bytesPerRow, bytesPerImage);
}

void GfxAccelHostUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, const uint8_t *data, int dataLen)
{
	GLMetalUploadTexture(ctx, texObj, level, width, height, data, dataLen);
}

struct GfxAccelHostArbProgram {
	int fragment;
	int metal;
	int invariant;
	int fog;
	int depth;
	int position;
	uint32_t targets[GFXACCELHOSTPROGRAMUNITS];
	std::map<std::string, std::string> names;
	std::map<std::string, std::string> arrays;
	std::map<std::string, int> offsets;
	std::string declarations;
	std::string body;
};

static std::string GfxAccelHostArbTrim(const std::string &text)
{
	size_t first;
	size_t last;
	first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return std::string();
	last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

static std::vector<std::string> GfxAccelHostArbSplit(const std::string &text, char separator)
{
	std::vector<std::string> pieces;
	size_t start;
	size_t index;
	int nesting;
	start = 0;
	nesting = 0;
	for (index = 0; index < text.size(); index++) {
		if (text[index] == '{' || text[index] == '[')
			nesting++;
		else if (text[index] == '}' || text[index] == ']')
			nesting--;
		else if (text[index] == separator && nesting == 0) {
			pieces.push_back(GfxAccelHostArbTrim(text.substr(start, index - start)));
			start = index + 1;
		}
	}
	pieces.push_back(GfxAccelHostArbTrim(text.substr(start)));
	return pieces;
}

static std::string GfxAccelHostArbNumber(int value)
{
	char digits[16];
	snprintf(digits, sizeof(digits), "%d", value);
	return std::string(digits);
}

static int GfxAccelHostArbPrefix(const std::string &text, size_t *position, const char *prefix)
{
	size_t length;
	length = strlen(prefix);
	if (text.compare(*position, length, prefix) != 0)
		return 0;
	*position += length;
	return 1;
}

static int GfxAccelHostArbIndex(const std::string &text, size_t *position, int *first, int *last)
{
	size_t close;
	size_t range;
	if (*position >= text.size() || text[*position] != '[')
		return 0;
	close = text.find(']', *position);
	if (close == std::string::npos)
		return 0;
	*first = atoi(text.c_str() + *position + 1);
	*last = *first;
	range = text.find("..", *position);
	if (range != std::string::npos && range < close)
		*last = atoi(text.c_str() + range + 2);
	*position = close + 1;
	return 1;
}

static std::string GfxAccelHostArbVector(const std::string &x, const std::string &y, const std::string &z, const std::string &w)
{
	return "float4(" + x + ", " + y + ", " + z + ", " + w + ")";
}

static std::string GfxAccelHostArbUniformName(GfxAccelHostArbProgram *program, const char *name)
{
	std::string prefix;
	if (program->metal)
		prefix = "programdata.";
	return prefix + name;
}

static std::string GfxAccelHostArbUniform(GfxAccelHostArbProgram *program, const char *name, int index)
{
	return GfxAccelHostArbUniformName(program, name) + "[" + GfxAccelHostArbNumber(index) + "]";
}

static std::string GfxAccelHostArbColumn(GfxAccelHostArbProgram *program, int base, int column)
{
	static const char *const components[4] = { ".x", ".y", ".z", ".w" };
	return GfxAccelHostArbVector(GfxAccelHostArbUniform(program, "State", base) + components[column], GfxAccelHostArbUniform(program, "State", base + 1) + components[column],
		GfxAccelHostArbUniform(program, "State", base + 2) + components[column], GfxAccelHostArbUniform(program, "State", base + 3) + components[column]);
}

static int GfxAccelHostArbMatrix(GfxAccelHostArbProgram *program, const std::string &text, size_t *position, std::vector<std::string> *rows)
{
	static const char *const names[3] = { "mvp", "modelview", "projection" };
	int base;
	int unit;
	int last;
	int first;
	int inverse;
	int transpose;
	int index;
	base = -1;
	for (index = 0; index < 3 && base < 0; index++)
		if (GfxAccelHostArbPrefix(text, position, names[index]))
			base = GFXACCELHOSTSTATEMATRICES + index * 4;
	if (base < 0 && GfxAccelHostArbPrefix(text, position, "texture")) {
		unit = 0;
		GfxAccelHostArbIndex(text, position, &unit, &last);
		if (unit < 0 || unit >= GFXACCELHOSTSTATETEXTUREMATRICES)
			return 0;
		base = GFXACCELHOSTSTATEMATRICES + 12 + unit * 4;
	}
	if (base < 0 || program->fragment)
		return 0;
	inverse = 0;
	transpose = 0;
	if (GfxAccelHostArbPrefix(text, position, ".inverse"))
		inverse = 1;
	else if (GfxAccelHostArbPrefix(text, position, ".transpose"))
		transpose = 1;
	else if (GfxAccelHostArbPrefix(text, position, ".invtrans")) {
		inverse = 1;
		transpose = 1;
	}
	if (inverse)
		base += GFXACCELHOSTSTATEINVERSE;
	first = 0;
	last = 3;
	if (GfxAccelHostArbPrefix(text, position, ".row") && !GfxAccelHostArbIndex(text, position, &first, &last))
		return 0;
	if (first < 0 || last > 3 || first > last)
		return 0;
	for (index = first; index <= last; index++) {
		if (transpose)
			rows->push_back(GfxAccelHostArbColumn(program, base, index));
		else
			rows->push_back(GfxAccelHostArbUniform(program, "State", base + index));
	}
	return 1;
}

static std::string GfxAccelHostArbVertexInput(int attribute)
{
	if (attribute < 0 || attribute >= GFXACCELHOSTPROGRAMATTRIBUTES)
		return "float4(0.0, 0.0, 0.0, 1.0)";
	return "input.attr" + GfxAccelHostArbNumber(attribute);
}

static int GfxAccelHostArbVertexItem(const std::string &text, size_t *position, std::vector<std::string> *items)
{
	int first;
	int last;
	first = 0;
	last = 0;
	if (GfxAccelHostArbPrefix(text, position, "vertex.position"))
		items->push_back(GfxAccelHostArbVertexInput(GFXACCELHOSTATTRIBUTEPOSITION));
	else if (GfxAccelHostArbPrefix(text, position, "vertex.color.secondary"))
		items->push_back(GfxAccelHostArbVertexInput(GFXACCELHOSTATTRIBUTESECONDARY));
	else if (GfxAccelHostArbPrefix(text, position, "vertex.color")) {
		GfxAccelHostArbPrefix(text, position, ".primary");
		items->push_back(GfxAccelHostArbVertexInput(GFXACCELHOSTATTRIBUTECOLOR));
	} else if (GfxAccelHostArbPrefix(text, position, "vertex.fogcoord"))
		items->push_back(GfxAccelHostArbVector(GfxAccelHostArbVertexInput(GFXACCELHOSTATTRIBUTEFOG) + ".x", "0.0", "0.0", "1.0"));
	else if (GfxAccelHostArbPrefix(text, position, "vertex.texcoord")) {
		GfxAccelHostArbIndex(text, position, &first, &last);
		if (first >= GFXACCELHOSTATTRIBUTETEXCOORDS)
			first = GFXACCELHOSTPROGRAMATTRIBUTES;
		else
			first += GFXACCELHOSTATTRIBUTETEXCOORD;
		items->push_back(GfxAccelHostArbVertexInput(first));
	} else if (GfxAccelHostArbPrefix(text, position, "vertex.attrib")) {
		if (!GfxAccelHostArbIndex(text, position, &first, &last))
			return 0;
		items->push_back(GfxAccelHostArbVertexInput(first));
	} else if (GfxAccelHostArbPrefix(text, position, "vertex.normal") || GfxAccelHostArbPrefix(text, position, "vertex.weight") || GfxAccelHostArbPrefix(text, position, "vertex.matrixindex")) {
		GfxAccelHostArbIndex(text, position, &first, &last);
		items->push_back(GfxAccelHostArbVertexInput(GFXACCELHOSTPROGRAMATTRIBUTES));
	} else
		return 0;
	return 1;
}

static int GfxAccelHostArbFragmentItem(const std::string &text, size_t *position, std::vector<std::string> *items)
{
	int first;
	int last;
	first = 0;
	if (GfxAccelHostArbPrefix(text, position, "fragment.color.secondary"))
		items->push_back("input.secondary");
	else if (GfxAccelHostArbPrefix(text, position, "fragment.color")) {
		GfxAccelHostArbPrefix(text, position, ".primary");
		items->push_back("input.color");
	} else if (GfxAccelHostArbPrefix(text, position, "fragment.texcoord")) {
		GfxAccelHostArbIndex(text, position, &first, &last);
		if (first < 0 || first >= GFXACCELHOSTPROGRAMTEXCOORDS)
			return 0;
		items->push_back("input.tex" + GfxAccelHostArbNumber(first));
	} else if (GfxAccelHostArbPrefix(text, position, "fragment.fogcoord"))
		items->push_back(GfxAccelHostArbVector("input.fog.x", "0.0", "0.0", "1.0"));
	else if (GfxAccelHostArbPrefix(text, position, "fragment.position"))
		items->push_back("input.position");
	else
		return 0;
	return 1;
}

static int GfxAccelHostArbItems(GfxAccelHostArbProgram *program, const std::string &text, size_t *position, std::vector<std::string> *items)
{
	const char *name;
	int first;
	int last;
	int index;
	int fog;
	name = NULL;
	if (GfxAccelHostArbPrefix(text, position, "program.env"))
		name = "Env";
	else if (GfxAccelHostArbPrefix(text, position, "program.local"))
		name = "Local";
	if (name) {
		if (!GfxAccelHostArbIndex(text, position, &first, &last) || first < 0 || last >= GFXACCELHOSTPROGRAMLOCALS || first > last)
			return 0;
		for (index = first; index <= last; index++)
			items->push_back(GfxAccelHostArbUniform(program, name, index));
		return 1;
	}
	if (GfxAccelHostArbPrefix(text, position, "state.matrix."))
		return GfxAccelHostArbMatrix(program, text, position, items);
	fog = GFXACCELHOSTSTATEVERTEXFOG;
	if (program->fragment)
		fog = GFXACCELHOSTSTATEFRAGMENTFOG;
	if (GfxAccelHostArbPrefix(text, position, "state.fog.color")) {
		items->push_back(GfxAccelHostArbUniform(program, "State", fog));
		return 1;
	}
	if (GfxAccelHostArbPrefix(text, position, "state.fog.params")) {
		items->push_back(GfxAccelHostArbUniform(program, "State", fog + 1));
		return 1;
	}
	if (GfxAccelHostArbPrefix(text, position, "state.texenv")) {
		first = 0;
		GfxAccelHostArbIndex(text, position, &first, &last);
		if (!program->fragment || !GfxAccelHostArbPrefix(text, position, ".color") || first < 0 || first >= GFXACCELHOSTPROGRAMUNITS)
			return 0;
		items->push_back(GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATETEXENV + first));
		return 1;
	}
	if (program->fragment)
		return GfxAccelHostArbFragmentItem(text, position, items);
	return GfxAccelHostArbVertexItem(text, position, items);
}

static std::string GfxAccelHostArbConstant(const std::string &text)
{
	std::vector<std::string> values;
	std::string value;
	if (text.empty() || text[0] != '{') {
		value = "float(" + text + ")";
		return GfxAccelHostArbVector(value, value, value, value);
	}
	values = GfxAccelHostArbSplit(text.substr(1, text.size() - 2), ',');
	while (values.size() < 4) {
		if (values.size() == 3)
			values.push_back("1.0");
		else
			values.push_back("0.0");
	}
	return GfxAccelHostArbVector("float(" + values[0] + ")", "float(" + values[1] + ")", "float(" + values[2] + ")", "float(" + values[3] + ")");
}

static std::string GfxAccelHostArbName(const std::string &name)
{
	return "u_" + name;
}

static int GfxAccelHostArbSwizzle(const std::string &text, std::string *swizzle)
{
	std::string result;
	size_t index;
	char letter;
	if (text.empty() || text.size() > 4)
		return 0;
	for (index = 0; index < text.size(); index++) {
		letter = text[index];
		if (letter == 'r')
			letter = 'x';
		else if (letter == 'g')
			letter = 'y';
		else if (letter == 'b')
			letter = 'z';
		else if (letter == 'a')
			letter = 'w';
		if (letter != 'x' && letter != 'y' && letter != 'z' && letter != 'w')
			return 0;
		result += letter;
	}
	*swizzle = result;
	return 1;
}

static int GfxAccelHostArbArrayIndex(GfxAccelHostArbProgram *program, const std::string &name, const std::string &inside, std::string *expression)
{
	std::string text;
	std::string address;
	std::string component;
	size_t operation;
	int offset;
	text = GfxAccelHostArbTrim(inside);
	offset = program->offsets[name];
	if (!text.empty() && (isalpha((unsigned char)text[0]) || text[0] == '_')) {
		operation = text.find_first_of("+-");
		address = GfxAccelHostArbTrim(text.substr(0, operation));
		if (address.size() < 3 || address.compare(address.size() - 2, 2, ".x") != 0 || program->names.find(address.substr(0, address.size() - 2)) == program->names.end())
			return 0;
		component = GfxAccelHostArbName(address.substr(0, address.size() - 2)) + ".x";
		if (operation != std::string::npos) {
			if (text[operation] == '-')
				offset -= atoi(text.c_str() + operation + 1);
			else
				offset += atoi(text.c_str() + operation + 1);
		}
		*expression = program->arrays[name] + "[" + component + " + " + GfxAccelHostArbNumber(offset) + "]";
		return 1;
	}
	*expression = program->arrays[name] + "[" + GfxAccelHostArbNumber(offset + atoi(text.c_str())) + "]";
	return 1;
}

static int GfxAccelHostArbSource(GfxAccelHostArbProgram *program, const std::string &raw, std::string *expression)
{
	std::vector<std::string> items;
	std::string text;
	std::string name;
	std::string swizzle;
	std::string value;
	size_t position;
	size_t close;
	int negate;
	text = GfxAccelHostArbTrim(raw);
	negate = 0;
	if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
		negate = text[0] == '-';
		text = GfxAccelHostArbTrim(text.substr(1));
	}
	if (text.empty())
		return 0;
	position = 0;
	if (text[0] == '{') {
		close = text.find('}');
		if (close == std::string::npos)
			return 0;
		value = GfxAccelHostArbConstant(text.substr(0, close + 1));
		position = close + 1;
	} else if (isdigit((unsigned char)text[0]) || text[0] == '.') {
		value = GfxAccelHostArbConstant(text);
		position = text.size();
	} else if (GfxAccelHostArbItems(program, text, &position, &items)) {
		if (items.size() != 1)
			return 0;
		value = items[0];
	} else {
		position = 0;
		while (position < text.size() && (isalnum((unsigned char)text[position]) || text[position] == '_'))
			position++;
		name = text.substr(0, position);
		if (position < text.size() && text[position] == '[') {
			close = text.find(']', position);
			if (close == std::string::npos || program->arrays.find(name) == program->arrays.end())
				return 0;
			if (!GfxAccelHostArbArrayIndex(program, name, text.substr(position + 1, close - position - 1), &value))
				return 0;
			position = close + 1;
		} else {
			if (program->names.find(name) == program->names.end())
				return 0;
			value = program->names[name];
		}
	}
	if (position < text.size()) {
		if (text[position] != '.' || !GfxAccelHostArbSwizzle(text.substr(position + 1), &swizzle) || (swizzle.size() != 1 && swizzle.size() != 4))
			return 0;
		if (swizzle.size() == 1)
			swizzle = swizzle + swizzle + swizzle + swizzle;
		value = "(" + value + ")." + swizzle;
	}
	if (negate)
		value = "(-" + value + ")";
	*expression = value;
	return 1;
}

static int GfxAccelHostArbDestination(GfxAccelHostArbProgram *program, const std::string &raw, std::string *destination, std::string *mask)
{
	static const char *const outputs[] = {
		"result.position", "resultposition", "result.color.secondary", "resultsecondary", "result.color.front.secondary", "resultsecondary",
		"result.color.back.secondary", "resultunused", "result.color.back.primary", "resultunused", "result.color.back", "resultunused",
		"result.color.front.primary", "resultcolor", "result.color.front", "resultcolor", "result.color.primary", "resultcolor", "result.color", "resultcolor",
		"result.fogcoord", "resultfog", "result.pointsize", "resultunused", "result.depth", "resultdepth"
	};
	std::string text;
	std::string name;
	size_t position;
	size_t index;
	int first;
	int last;
	text = GfxAccelHostArbTrim(raw);
	position = 0;
	name.clear();
	for (index = 0; index < sizeof(outputs) / sizeof(outputs[0]) && name.empty(); index += 2)
		if (GfxAccelHostArbPrefix(text, &position, outputs[index]))
			name = outputs[index + 1];
	if (name.empty() && GfxAccelHostArbPrefix(text, &position, "result.texcoord")) {
		first = 0;
		GfxAccelHostArbIndex(text, &position, &first, &last);
		if (first < 0 || first >= GFXACCELHOSTPROGRAMTEXCOORDS)
			return 0;
		name = "resulttex" + GfxAccelHostArbNumber(first);
	}
	if (name.empty()) {
		while (position < text.size() && (isalnum((unsigned char)text[position]) || text[position] == '_'))
			position++;
		if (program->names.find(text.substr(0, position)) == program->names.end())
			return 0;
		name = program->names[text.substr(0, position)];
	}
	if (name == "resultdepth")
		program->depth = 1;
	if (name == "resultposition")
		program->position = 1;
	mask->clear();
	if (position < text.size()) {
		if (text[position] != '.' || !GfxAccelHostArbSwizzle(text.substr(position + 1), mask))
			return 0;
		*mask = "." + *mask;
	}
	*destination = name;
	return 1;
}

static std::string GfxAccelHostArbSwizzleComponent(const std::string &component, const std::string &source)
{
	std::string text;
	std::string sign;
	std::string swizzle;
	text = GfxAccelHostArbTrim(component);
	if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
		if (text[0] == '-')
			sign = "-";
		text = text.substr(1);
	}
	if (text == "0" || text == "1")
		return sign + text + ".0";
	if (!GfxAccelHostArbSwizzle(text, &swizzle))
		return "0.0";
	return sign + "(" + source + ")." + swizzle;
}

static std::string GfxAccelHostArbReplicate(const std::string &scalar)
{
	return "Replicate(" + scalar + ")";
}

static int GfxAccelHostArbTexture(GfxAccelHostArbProgram *program, const std::string &opcode, const std::string &coordinate, const std::string &unittext, const std::string &targettext, std::string *value)
{
	std::string coords;
	std::string image;
	std::string filter;
	std::string size;
	uint32_t target;
	size_t position;
	int unit;
	int last;
	position = 0;
	if (!GfxAccelHostArbPrefix(unittext, &position, "texture") || !GfxAccelHostArbIndex(unittext, &position, &unit, &last) || unit < 0 || unit >= GFXACCELHOSTPROGRAMUNITS)
		return 0;
	if (targettext == "2D" || targettext == "1D")
		target = GFXACCELHOSTTEXTURE2D;
	else if (targettext == "RECT")
		target = GFXACCELHOSTTEXTURERECTANGLE;
	else if (targettext == "CUBE")
		target = GFXACCELHOSTCUBEMAP;
	else if (targettext == "3D")
		target = GFXACCELHOSTTEXTURE3D;
	else
		return 0;
	if (program->targets[unit] && program->targets[unit] != target)
		return 0;
	program->targets[unit] = target;
	coords = coordinate;
	if (opcode == "TXP")
		coords = "((" + coordinate + ") / (" + coordinate + ").w)";
	if (target == GFXACCELHOSTTEXTURE2D || target == GFXACCELHOSTTEXTURERECTANGLE)
		coords = "(" + coords + ").xy";
	else
		coords = "(" + coords + ").xyz";
	if (target == GFXACCELHOSTTEXTURERECTANGLE)
		coords = "(" + coords + " / " + GfxAccelHostArbUniform(program, "Size", unit) + ".xy)";
	image = "Image" + GfxAccelHostArbNumber(unit);
	filter = "Filter" + GfxAccelHostArbNumber(unit);
	if (program->metal && opcode == "TXB")
		*value = image + ".sample(" + filter + ", " + coords + ", bias((" + coordinate + ").w))";
	else if (program->metal)
		*value = image + ".sample(" + filter + ", " + coords + ")";
	else if (opcode == "TXB")
		*value = image + ".SampleBias(" + filter + ", " + coords + ", (" + coordinate + ").w)";
	else
		*value = image + ".Sample(" + filter + ", " + coords + ")";
	return 1;
}

static int GfxAccelHostArbOperation(const std::string &opcode, const std::vector<std::string> &sources, std::string *value)
{
	std::string a;
	std::string b;
	std::string c;
	a = sources[0];
	if (sources.size() > 1)
		b = sources[1];
	if (sources.size() > 2)
		c = sources[2];
	if (opcode == "MOV")
		*value = a;
	else if (opcode == "ABS")
		*value = "abs(" + a + ")";
	else if (opcode == "ADD")
		*value = "(" + a + " + " + b + ")";
	else if (opcode == "SUB")
		*value = "(" + a + " - " + b + ")";
	else if (opcode == "MUL")
		*value = "(" + a + " * " + b + ")";
	else if (opcode == "MAD")
		*value = "(" + a + " * " + b + " + " + c + ")";
	else if (opcode == "MAX")
		*value = "max(" + a + ", " + b + ")";
	else if (opcode == "MIN")
		*value = "min(" + a + ", " + b + ")";
	else if (opcode == "LRP")
		*value = "lerp(" + c + ", " + b + ", " + a + ")";
	else if (opcode == "FRC")
		*value = "frac(" + a + ")";
	else if (opcode == "FLR")
		*value = "floor(" + a + ")";
	else if (opcode == "SGE")
		*value = "step(" + b + ", " + a + ")";
	else if (opcode == "SLT")
		*value = "(float4(1.0, 1.0, 1.0, 1.0) - step(" + b + ", " + a + "))";
	else if (opcode == "CMP")
		*value = "lerp(" + b + ", " + c + ", step(float4(0.0, 0.0, 0.0, 0.0), " + a + "))";
	else if (opcode == "DP3")
		*value = GfxAccelHostArbReplicate("dot((" + a + ").xyz, (" + b + ").xyz)");
	else if (opcode == "DP4")
		*value = GfxAccelHostArbReplicate("dot(" + a + ", " + b + ")");
	else if (opcode == "DPH")
		*value = GfxAccelHostArbReplicate("dot(float4((" + a + ").xyz, 1.0), " + b + ")");
	else if (opcode == "DST")
		*value = GfxAccelHostArbVector("1.0", "(" + a + ").y * (" + b + ").y", "(" + a + ").z", "(" + b + ").w");
	else if (opcode == "XPD")
		*value = "float4(cross((" + a + ").xyz, (" + b + ").xyz), 1.0)";
	else if (opcode == "POW")
		*value = GfxAccelHostArbReplicate("pow(abs((" + a + ").x), (" + b + ").x)");
	else if (opcode == "RCP")
		*value = GfxAccelHostArbReplicate("1.0 / (" + a + ").x");
	else if (opcode == "RSQ")
		*value = GfxAccelHostArbReplicate("rsqrt(abs((" + a + ").x))");
	else if (opcode == "LG2")
		*value = GfxAccelHostArbReplicate("log2(abs((" + a + ").x))");
	else if (opcode == "EX2")
		*value = GfxAccelHostArbReplicate("exp2((" + a + ").x)");
	else if (opcode == "SIN")
		*value = GfxAccelHostArbReplicate("sin((" + a + ").x)");
	else if (opcode == "COS")
		*value = GfxAccelHostArbReplicate("cos((" + a + ").x)");
	else if (opcode == "SCS")
		*value = GfxAccelHostArbVector("cos((" + a + ").x)", "sin((" + a + ").x)", "0.0", "1.0");
	else if (opcode == "EXP")
		*value = GfxAccelHostArbVector("exp2(floor((" + a + ").x))", "frac((" + a + ").x)", "exp2((" + a + ").x)", "1.0");
	else if (opcode == "LOG")
		*value = GfxAccelHostArbVector("floor(log2(abs((" + a + ").x)))", "abs((" + a + ").x) / exp2(floor(log2(abs((" + a + ").x))))", "log2(abs((" + a + ").x))", "1.0");
	else if (opcode == "LIT")
		*value = GfxAccelHostArbVector("1.0", "max((" + a + ").x, 0.0)", "max(sign((" + a + ").x), 0.0) * pow(max((" + a + ").y, 0.0), clamp((" + a + ").w, -128.0, 128.0))", "1.0");
	else
		return 0;
	return 1;
}

static int GfxAccelHostArbInstruction(GfxAccelHostArbProgram *program, const std::string &statement)
{
	std::vector<std::string> arguments;
	std::vector<std::string> sources;
	std::string opcode;
	std::string destination;
	std::string mask;
	std::string value;
	std::string source;
	size_t space;
	size_t index;
	int saturate;
	space = statement.find_first_of(" \t");
	if (space == std::string::npos)
		return 0;
	opcode = statement.substr(0, space);
	arguments = GfxAccelHostArbSplit(statement.substr(space + 1), ',');
	saturate = 0;
	if (opcode.size() > 4 && opcode.compare(opcode.size() - 4, 4, "_SAT") == 0) {
		saturate = 1;
		opcode = opcode.substr(0, opcode.size() - 4);
	}
	if (opcode == "KIL") {
		if (!GfxAccelHostArbSource(program, arguments[0], &source))
			return 0;
		program->body += "\tif (any((" + source + ") < float4(0.0, 0.0, 0.0, 0.0)))\n\t\tDiscardFragment;\n";
		return 1;
	}
	if (opcode == "ARL") {
		if (arguments.size() != 2 || !GfxAccelHostArbSource(program, arguments[1], &source))
			return 0;
		destination = GfxAccelHostArbTrim(arguments[0]);
		if (destination.size() < 3 || program->names.find(destination.substr(0, destination.size() - 2)) == program->names.end())
			return 0;
		program->body += "\t" + program->names[destination.substr(0, destination.size() - 2)] + ".x = (int)floor((" + source + ").x);\n";
		return 1;
	}
	if (arguments.size() < 2 || !GfxAccelHostArbDestination(program, arguments[0], &destination, &mask))
		return 0;
	if (opcode == "SWZ") {
		if (arguments.size() != 6 || !GfxAccelHostArbSource(program, arguments[1], &source))
			return 0;
		value = GfxAccelHostArbVector(GfxAccelHostArbSwizzleComponent(arguments[2], source), GfxAccelHostArbSwizzleComponent(arguments[3], source),
			GfxAccelHostArbSwizzleComponent(arguments[4], source), GfxAccelHostArbSwizzleComponent(arguments[5], source));
	} else if (opcode == "TEX" || opcode == "TXP" || opcode == "TXB") {
		if (arguments.size() != 4 || !program->fragment || !GfxAccelHostArbSource(program, arguments[1], &source) ||
			!GfxAccelHostArbTexture(program, opcode, source, arguments[2], arguments[3], &value))
			return 0;
	} else {
		for (index = 1; index < arguments.size(); index++) {
			if (!GfxAccelHostArbSource(program, arguments[index], &source))
				return 0;
			sources.push_back(source);
		}
		if (!GfxAccelHostArbOperation(opcode, sources, &value))
			return 0;
	}
	if (saturate)
		value = "saturate(" + value + ")";
	if (mask.empty())
		program->body += "\t" + destination + " = " + value + ";\n";
	else
		program->body += "\t" + destination + mask + " = (" + value + ")" + mask + ";\n";
	return 1;
}

static int GfxAccelHostArbArray(GfxAccelHostArbProgram *program, const std::string &name, const std::string &value)
{
	std::vector<std::string> pieces;
	std::vector<std::string> items;
	std::string text;
	size_t index;
	size_t position;
	size_t range;
	int first;
	int last;
	text = GfxAccelHostArbTrim(value);
	if (text.size() < 2 || text[0] != '{' || text[text.size() - 1] != '}')
		return 0;
	pieces = GfxAccelHostArbSplit(text.substr(1, text.size() - 2), ',');
	if (pieces.size() == 1) {
		position = 0;
		range = 0;
		if (GfxAccelHostArbPrefix(pieces[0], &position, "program.env"))
			program->arrays[name] = GfxAccelHostArbUniformName(program, "Env");
		else if (GfxAccelHostArbPrefix(pieces[0], &position, "program.local"))
			program->arrays[name] = GfxAccelHostArbUniformName(program, "Local");
		else
			range = 1;
		if (!range) {
			if (!GfxAccelHostArbIndex(pieces[0], &position, &first, &last) || position != pieces[0].size())
				return 0;
			program->offsets[name] = first;
			return 1;
		}
	}
	for (index = 0; index < pieces.size(); index++) {
		position = 0;
		if (!pieces[index].empty() && (pieces[index][0] == '{' || isdigit((unsigned char)pieces[index][0]) || pieces[index][0] == '-' || pieces[index][0] == '.'))
			items.push_back(GfxAccelHostArbConstant(pieces[index]));
		else if (!GfxAccelHostArbItems(program, pieces[index], &position, &items) || position != pieces[index].size())
			return 0;
	}
	program->declarations += "\tfloat4 " + GfxAccelHostArbName(name) + "[" + GfxAccelHostArbNumber((int)items.size()) + "] = { ";
	for (index = 0; index < items.size(); index++) {
		if (index)
			program->declarations += ", ";
		program->declarations += items[index];
	}
	program->declarations += " };\n";
	program->arrays[name] = GfxAccelHostArbName(name);
	program->offsets[name] = 0;
	return 1;
}

static int GfxAccelHostArbDeclaration(GfxAccelHostArbProgram *program, const std::string &statement)
{
	std::vector<std::string> names;
	std::vector<std::string> items;
	std::string keyword;
	std::string name;
	std::string value;
	std::string expression;
	std::string mask;
	size_t space;
	size_t equals;
	size_t bracket;
	size_t index;
	size_t position;
	space = statement.find_first_of(" \t");
	keyword = statement.substr(0, space);
	if (keyword == "TEMP" || keyword == "ADDRESS") {
		names = GfxAccelHostArbSplit(statement.substr(space + 1), ',');
		for (index = 0; index < names.size(); index++) {
			program->names[names[index]] = GfxAccelHostArbName(names[index]);
			if (keyword == "TEMP")
				program->declarations += "\tfloat4 " + GfxAccelHostArbName(names[index]) + " = float4(0.0, 0.0, 0.0, 0.0);\n";
			else
				program->declarations += "\tint4 " + GfxAccelHostArbName(names[index]) + " = int4(0, 0, 0, 0);\n";
		}
		return 1;
	}
	equals = statement.find('=');
	if (equals == std::string::npos)
		return 0;
	name = GfxAccelHostArbTrim(statement.substr(space, equals - space));
	value = GfxAccelHostArbTrim(statement.substr(equals + 1));
	bracket = name.find('[');
	if (keyword == "ALIAS") {
		if (program->names.find(value) != program->names.end())
			program->names[name] = program->names[value];
		else if (program->arrays.find(value) != program->arrays.end()) {
			program->arrays[name] = program->arrays[value];
			program->offsets[name] = program->offsets[value];
		} else
			return 0;
		return 1;
	}
	if (keyword == "PARAM" && bracket != std::string::npos)
		return GfxAccelHostArbArray(program, GfxAccelHostArbTrim(name.substr(0, bracket)), value);
	if (keyword == "OUTPUT") {
		if (!GfxAccelHostArbDestination(program, value, &expression, &mask) || !mask.empty())
			return 0;
		program->names[name] = expression;
		return 1;
	}
	position = 0;
	if (keyword == "PARAM" && !value.empty() && (value[0] == '{' || isdigit((unsigned char)value[0]) || value[0] == '-' || value[0] == '.'))
		expression = GfxAccelHostArbConstant(value);
	else if ((keyword == "PARAM" || keyword == "ATTRIB") && GfxAccelHostArbItems(program, value, &position, &items) && items.size() == 1 && position == value.size())
		expression = items[0];
	else
		return 0;
	program->names[name] = GfxAccelHostArbName(name);
	program->declarations += "\tfloat4 " + GfxAccelHostArbName(name) + " = " + expression + ";\n";
	return 1;
}

static int GfxAccelHostArbOption(GfxAccelHostArbProgram *program, const std::string &option)
{
	if (option == "ARB_position_invariant" && !program->fragment)
		program->invariant = 1;
	else if (option == "ARB_fog_linear" && program->fragment)
		program->fog = GFXACCELHOSTFOGLINEAR;
	else if (option == "ARB_fog_exp" && program->fragment)
		program->fog = GFXACCELHOSTFOGEXP;
	else if (option == "ARB_fog_exp2" && program->fragment)
		program->fog = GFXACCELHOSTFOGEXP2;
	else if (option != "ARB_precision_hint_fastest" && option != "ARB_precision_hint_nicest")
		return 0;
	return 1;
}

static int GfxAccelHostArbParse(GfxAccelHostArbProgram *program, const char *programtext, uint32_t length)
{
	std::vector<std::string> statements;
	std::string source;
	std::string stripped;
	std::string keyword;
	size_t index;
	size_t comment;
	size_t end;
	source.assign(programtext, length);
	index = 0;
	while (index < source.size()) {
		end = source.find('\n', index);
		if (end == std::string::npos)
			end = source.size();
		comment = source.find('#', index);
		if (comment != std::string::npos && comment < end)
			stripped += source.substr(index, comment - index) + " ";
		else
			stripped += source.substr(index, end - index) + " ";
		index = end + 1;
	}
	index = stripped.find("!!");
	if (index != std::string::npos)
		stripped = stripped.substr(stripped.find_first_of(" \t\r\n", index));
	statements = GfxAccelHostArbSplit(stripped, ';');
	for (index = 0; index < statements.size(); index++) {
		const std::string &statement = statements[index];
		if (statement.empty() || statement == "END")
			continue;
		keyword = statement.substr(0, statement.find_first_of(" \t"));
		if (keyword == "OPTION") {
			if (!GfxAccelHostArbOption(program, GfxAccelHostArbTrim(statement.substr(6))))
				return 0;
		} else if (keyword == "TEMP" || keyword == "ADDRESS" || keyword == "PARAM" || keyword == "ATTRIB" || keyword == "OUTPUT" || keyword == "ALIAS") {
			if (!GfxAccelHostArbDeclaration(program, statement))
				return 0;
		} else if (!GfxAccelHostArbInstruction(program, statement))
			return 0;
	}
	return 1;
}

static void GfxAccelHostArbBegin(GfxAccelHostArbProgram *program, int fragment, int metal)
{
	program->fragment = fragment;
	program->metal = metal;
	program->invariant = 0;
	program->fog = 0;
	program->depth = 0;
	program->position = 0;
	memset(program->targets, 0, sizeof(program->targets));
}

int GfxAccelHostProgramTextureTargets(const char *programtext, uint32_t length, uint32_t *targets)
{
	GfxAccelHostArbProgram program;
	GfxAccelHostArbBegin(&program, 1, 0);
	if (!GfxAccelHostArbParse(&program, programtext, length))
		return 0;
	memcpy(targets, program.targets, sizeof(program.targets));
	return 1;
}

static const char GfxAccelHostArbAlphaPass[] =
	"bool AlphaPass(float alpha, int func, float reference)\n{\n\tbool pass = true;\n\tif (func == 512) pass = false;\n\tif (func == 513) pass = alpha < reference;\n"
	"\tif (func == 514) pass = alpha == reference;\n\tif (func == 515) pass = alpha <= reference;\n\tif (func == 516) pass = alpha > reference;\n"
	"\tif (func == 517) pass = alpha != reference;\n\tif (func == 518) pass = alpha >= reference;\n\treturn pass;\n}\n";

static std::string GfxAccelHostArbPrelude(GfxAccelHostArbProgram *program)
{
	std::string text;
	std::string unit;
	std::string type;
	int index;
	if (program->metal) {
		text = "#include <metal_stdlib>\nusing namespace metal;\n#define lerp mix\n#define frac fract\n#define DiscardFragment discard_fragment()\n";
		text += "float4 Replicate(float value) { return float4(value, value, value, value); }\n";
		text += "struct ProgramVarying { float4 position [[position]]; float4 color; float4 secondary; float4 fog;";
		for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++)
			text += " float4 tex" + GfxAccelHostArbNumber(index) + ";";
		text += " };\n";
	} else {
		text = "#define DiscardFragment discard\nfloat4 Replicate(float value) { return float4(value, value, value, value); }\n";
		text += "struct ProgramVarying { float4 position : SV_Position; float4 color : TEXCOORD0; float4 secondary : TEXCOORD1; float4 fog : TEXCOORD2;";
		for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++)
			text += " float4 tex" + GfxAccelHostArbNumber(index) + " : TEXCOORD" + GfxAccelHostArbNumber(index + 3) + ";";
		text += " };\n";
	}
	if (!program->fragment) {
		if (program->metal) {
			text += "struct ProgramData { float4 Env[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) + "]; float4 Local[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) +
				"]; float4 State[" + GfxAccelHostArbNumber(GFXACCELHOSTSTATEVERTEXCOUNT) + "]; };\nstruct ProgramAttributes {";
			for (index = 0; index < GFXACCELHOSTPROGRAMATTRIBUTES; index++)
				text += " float4 attr" + GfxAccelHostArbNumber(index) + " [[attribute(" + GfxAccelHostArbNumber(index) + ")]];";
			text += " };\nvertex ProgramVarying VSMain(ProgramAttributes input [[stage_in]], constant ProgramData &programdata [[buffer(0)]])\n{\n";
		} else {
			text += "cbuffer ProgramData : register(b0, space1) { float4 Env[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) + "]; float4 Local[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) +
				"]; float4 State[" + GfxAccelHostArbNumber(GFXACCELHOSTSTATEVERTEXCOUNT) + "]; };\nstruct ProgramAttributes {";
			for (index = 0; index < GFXACCELHOSTPROGRAMATTRIBUTES; index++)
				text += " float4 attr" + GfxAccelHostArbNumber(index) + " : TEXCOORD" + GfxAccelHostArbNumber(index) + ";";
			text += " };\nProgramVarying VSMain(ProgramAttributes input)\n{\n";
		}
		return text;
	}
	text += GfxAccelHostArbAlphaPass;
	if (program->metal) {
		text += "struct ProgramData { float4 Env[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) + "]; float4 Local[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) +
			"]; float4 Size[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMUNITS) + "]; float4 State[" + GfxAccelHostArbNumber(GFXACCELHOSTSTATEFRAGMENTCOUNT) + "]; };\n";
		text += "struct ProgramOutput { float4 color [[color(0)]];";
		if (program->depth)
			text += " float depth [[depth(any)]];";
		text += " };\nfragment ProgramOutput PSMain(ProgramVarying input [[stage_in]], constant ProgramData &programdata [[buffer(0)]]";
		for (index = 0; index < GFXACCELHOSTPROGRAMUNITS; index++) {
			unit = GfxAccelHostArbNumber(index);
			type = "texture2d<float>";
			if (program->targets[index] == GFXACCELHOSTCUBEMAP)
				type = "texturecube<float>";
			else if (program->targets[index] == GFXACCELHOSTTEXTURE3D)
				type = "texture3d<float>";
			text += ", " + type + " Image" + unit + " [[texture(" + unit + ")]], sampler Filter" + unit + " [[sampler(" + unit + ")]]";
		}
		text += ")\n{\n";
		return text;
	}
	text += "cbuffer ProgramData : register(b0, space3) { float4 Env[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) + "]; float4 Local[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMLOCALS) +
		"]; float4 Size[" + GfxAccelHostArbNumber(GFXACCELHOSTPROGRAMUNITS) + "]; float4 State[" + GfxAccelHostArbNumber(GFXACCELHOSTSTATEFRAGMENTCOUNT) + "]; };\n";
	for (index = 0; index < GFXACCELHOSTPROGRAMUNITS; index++) {
		unit = GfxAccelHostArbNumber(index);
		type = "Texture2D";
		if (program->targets[index] == GFXACCELHOSTCUBEMAP)
			type = "TextureCube";
		else if (program->targets[index] == GFXACCELHOSTTEXTURE3D)
			type = "Texture3D";
		text += "[[vk::combinedImageSampler]] " + type + " Image" + unit + " : register(t" + unit + ", space2); [[vk::combinedImageSampler]] SamplerState Filter" + unit + " : register(s" + unit + ", space2);\n";
	}
	text += "struct ProgramOutput { float4 color : SV_Target0;";
	if (program->depth)
		text += " float depth : SV_Depth;";
	text += " };\nProgramOutput PSMain(ProgramVarying input)\n{\n";
	return text;
}

static std::string GfxAccelHostArbVertexEpilogue(GfxAccelHostArbProgram *program)
{
	std::string text;
	std::string zero;
	int index;
	text = "\tProgramVarying output;\n";
	if (program->invariant)
		text += "\tresultposition = float4(dot(" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEMATRICES) + ", input.attr0), dot(" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEMATRICES + 1) +
			", input.attr0), dot(" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEMATRICES + 2) + ", input.attr0), dot(" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEMATRICES + 3) + ", input.attr0));\n";
	if (program->invariant || program->position)
		text += "\toutput.position = float4(resultposition.x, resultposition.y, (resultposition.z + resultposition.w) * 0.5, resultposition.w);\n";
	else
		text += "\toutput.position = input.attr0;\n";
	text += "\toutput.color = saturate(resultcolor);\n\toutput.secondary = saturate(resultsecondary);\n\toutput.fog = resultfog;\n";
	for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++)
		text += "\toutput.tex" + GfxAccelHostArbNumber(index) + " = resulttex" + GfxAccelHostArbNumber(index) + ";\n";
	zero = GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEVERTEXZERO) + ".x";
	text += "\toutput.color += resultunused * " + zero + ";\n\treturn output;\n}\n";
	return text;
}

static std::string GfxAccelHostArbFragmentEpilogue(GfxAccelHostArbProgram *program)
{
	std::string text;
	std::string fog;
	std::string alpha;
	std::string coordinate;
	std::string sample;
	int index;
	fog = GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTFOG);
	alpha = GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEALPHA);
	if (program->fog == GFXACCELHOSTFOGLINEAR)
		text += "\tresultcolor.xyz = lerp(" + fog + ".xyz, resultcolor.xyz, saturate((" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTFOG + 1) +
			".z - input.fog.x) * " + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTFOG + 1) + ".w));\n";
	else if (program->fog == GFXACCELHOSTFOGEXP)
		text += "\tresultcolor.xyz = lerp(" + fog + ".xyz, resultcolor.xyz, saturate(exp(-" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTFOG + 1) + ".x * input.fog.x)));\n";
	else if (program->fog == GFXACCELHOSTFOGEXP2)
		text += "\tresultcolor.xyz = lerp(" + fog + ".xyz, resultcolor.xyz, saturate(exp(-pow(" + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTFOG + 1) + ".x * input.fog.x, 2.0))));\n";
	text += "\tif (" + alpha + ".z != 0.0 && !AlphaPass(resultcolor.w, (int)" + alpha + ".x, " + alpha + ".y))\n\t\tDiscardFragment;\n";
	text += "\tProgramOutput output;\n\toutput.color = resultcolor + (input.color + input.secondary + input.fog";
	for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++)
		text += " + input.tex" + GfxAccelHostArbNumber(index);
	text += ") * " + GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTZERO) + ".x;\n";
	for (index = 0; index < GFXACCELHOSTPROGRAMUNITS; index++) {
		coordinate = "float2(0.0, 0.0)";
		if (program->targets[index] == GFXACCELHOSTCUBEMAP || program->targets[index] == GFXACCELHOSTTEXTURE3D)
			coordinate = "float3(1.0, 0.0, 0.0)";
		sample = ".Sample(";
		if (program->metal)
			sample = ".sample(";
		text += "\toutput.color += Image" + GfxAccelHostArbNumber(index) + sample + "Filter" + GfxAccelHostArbNumber(index) + ", " + coordinate + ") * " +
			GfxAccelHostArbUniform(program, "State", GFXACCELHOSTSTATEFRAGMENTZERO) + ".x;\n";
	}
	if (program->depth)
		text += "\toutput.depth = resultdepth.z;\n";
	text += "\treturn output;\n}\n";
	return text;
}

char *GfxAccelHostTranslateProgram(const char *programtext, uint32_t length, int fragmentprogram, int metalshading)
{
	GfxAccelHostArbProgram program;
	std::string output;
	std::string results;
	char *copy;
	int index;
	GfxAccelHostArbBegin(&program, fragmentprogram, metalshading);
	if (!GfxAccelHostArbParse(&program, programtext, length))
		return NULL;
	output = GfxAccelHostArbPrelude(&program);
	if (fragmentprogram) {
		results = "\tfloat4 resultcolor = float4(0.0, 0.0, 0.0, 1.0);\n\tfloat4 resultdepth = float4(0.0, 0.0, input.position.z, 1.0);\n";
	} else if (program.body.empty() && !program.invariant) {
		results = "\tfloat4 resultposition = input.attr0;\n\tfloat4 resultcolor = input.attr1;\n\tfloat4 resultsecondary = input.attr2;\n\tfloat4 resultfog = input.attr5;\n\tfloat4 resultunused = float4(0.0, 0.0, 0.0, 0.0);\n";
		for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++) {
			if (index < GFXACCELHOSTATTRIBUTETEXCOORDS)
				results += "\tfloat4 resulttex" + GfxAccelHostArbNumber(index) + " = input.attr" + GfxAccelHostArbNumber(GFXACCELHOSTATTRIBUTETEXCOORD + index) + ";\n";
			else
				results += "\tfloat4 resulttex" + GfxAccelHostArbNumber(index) + " = float4(0.0, 0.0, 0.0, 1.0);\n";
		}
	} else {
		results = "\tfloat4 resultposition = float4(0.0, 0.0, 0.0, 1.0);\n\tfloat4 resultcolor = float4(0.0, 0.0, 0.0, 1.0);\n\tfloat4 resultsecondary = float4(0.0, 0.0, 0.0, 1.0);\n"
			"\tfloat4 resultfog = float4(0.0, 0.0, 0.0, 1.0);\n\tfloat4 resultunused = float4(0.0, 0.0, 0.0, 0.0);\n";
		for (index = 0; index < GFXACCELHOSTPROGRAMTEXCOORDS; index++)
			results += "\tfloat4 resulttex" + GfxAccelHostArbNumber(index) + " = float4(0.0, 0.0, 0.0, 1.0);\n";
	}
	output += results + program.declarations + program.body;
	if (fragmentprogram)
		output += GfxAccelHostArbFragmentEpilogue(&program);
	else
		output += GfxAccelHostArbVertexEpilogue(&program);
	copy = (char *)malloc(output.size() + 1);
	if (copy)
		memcpy(copy, output.c_str(), output.size() + 1);
	return copy;
}
