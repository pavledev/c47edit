#include "texture.h"

#include <cassert>
#include <filesystem>
#include <functional>
#include <map>

#include "global.h"
#include "chunk.h"
#include "gameobj.h"
#include "ByteWriter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include <stb_image.h>
#include <stb_image_write.h>
#include "squish.h"

std::map<uint32_t, void*> texmap;

void GlifyTexture(Chunk* c) {
	uint8_t* d = (uint8_t*)c->maindata.data();
	uint32_t texid = *(uint32_t*)d;
	int texh = *(uint16_t*)(d + 4);
	int texw = *(uint16_t*)(d + 6);
	int nmipmaps = *(uint16_t*)(d + 8);
	uint8_t* firstbmp = d + 20;
	while (*(firstbmp++));

	uint32_t pal[256];
	if (c->tag == 'PALN')
	{
		uint8_t* pnt = firstbmp;
		for (int m = 0; m < nmipmaps; m++)
			pnt += *(uint32_t*)pnt + 4;
		uint32_t npalentries = *(uint32_t*)pnt; pnt += 4;
		if (npalentries > 256) npalentries = 256;
		memcpy(pal, pnt, 4 * npalentries);
	}

	GLuint gltex;
	glGenTextures(1, &gltex);
	glBindTexture(GL_TEXTURE_2D, gltex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (nmipmaps > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	texmap[texid] = (void*)(uintptr_t)gltex;

	uint8_t* bmp = firstbmp;
	for (int m = 0; m < nmipmaps; m++)
	{
		uint32_t mmsize = *(uint32_t*)bmp; bmp += 4;
		int mmWidth = std::max(texw >> m, 1);
		int mmHeight = std::max(texh >> m, 1);
		if (c->tag == 'PALN')
		{
			uint32_t* pix32 = new uint32_t[mmsize];
			for (uint32_t p = 0; p < mmsize; p++)
				pix32[p] = pal[bmp[p]];
			glTexImage2D(GL_TEXTURE_2D, m, 4, mmWidth, mmHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix32);
			delete[] pix32;
		}
		else if (c->tag == 'RGBA')
			glTexImage2D(GL_TEXTURE_2D, m, 4, mmWidth, mmHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp);
		else
			ferr("Unknown texture format in Pack(Repeat).PAL.");
		bmp += mmsize;
	}
}

void GlifyAllTextures()
{
	for (Chunk& c : g_scene.palPack.subchunks)
		GlifyTexture(&c);
	for (Chunk& c : g_scene.lgtPack.subchunks)
		GlifyTexture(&c);

	glBindTexture(GL_TEXTURE_2D, 0);
}

void InvalidateTexture(uint32_t texid)
{
	GLuint gltex = (GLuint)(uintptr_t)texmap.at(texid);
	glDeleteTextures(1, &gltex);
	GlifyTexture(FindTextureChunk(g_scene, texid).first);
}

void UncacheAllTextures()
{
	for (auto& [id, vtex] : texmap) {
		GLuint gltex = (GLuint)(uintptr_t)vtex;
		glDeleteTextures(1, &gltex);
	}
	texmap.clear();
}

std::tuple<uint32_t, Chunk*, Chunk*> AddUninitializedTexture(Scene& scene)
{
	uint32_t texId = ++scene.numTextures;
	Chunk& chk = scene.palPack.subchunks.emplace_back();
	Chunk& dxtchk = scene.dxtPack.subchunks.emplace_back();
	return { texId, &chk, &dxtchk };
}

uint32_t AddTexture(Scene& scene, uint8_t* pixels, int width, int height, std::string_view name)
{
	auto [id, chk, dxtchk] = AddUninitializedTexture(scene);
	ImportTexture(pixels, width, height, name, *chk, *dxtchk, id);
	return id;
}

uint32_t AddTexture(Scene& scene, const std::filesystem::path& filepath)
{
	auto [id, chk, dxtchk] = AddUninitializedTexture(scene);
	ImportTexture(filepath, *chk, *dxtchk, id);
	return id;
}

uint32_t AddTexture(Scene& scene, const void* mem, size_t memSize, std::string_view name)
{
	auto [id, chk, dxtchk] = AddUninitializedTexture(scene);
	ImportTexture(mem, memSize, name, *chk, *dxtchk, id);
	return id;
}

void ImportTexture(uint8_t* pixels, int width, int height, std::string_view name, Chunk& chk, Chunk& dxtchk, int texid)
{
	int numMipmaps = 1;
	int flags = 0x14;
	int random = 0x12345678;

	ByteWriter<std::vector<uint8_t>> chkdata, dxtdata;
	chkdata.addU32(texid);
	chkdata.addU16(height);
	chkdata.addU16(width);
	chkdata.addU32(numMipmaps);
	chkdata.addU32(flags);
	chkdata.addU32(random);
	chkdata.addStringNT(name);
	dxtdata = chkdata;

	int size = width * height * 4;
	chkdata.addS32(size);
	chkdata.addData(pixels, size);

	// 1 COPY >_<
	chk.tag = 'RGBA';
	auto str = chkdata.take();
	chk.maindata.resize(str.size());
	memcpy(chk.maindata.data(), str.data(), str.size());

	// DXT
	size = squish::GetStorageRequirements(width, height, squish::kDxt1);
	dxtdata.addS32(size);
	uint8_t* comp = dxtdata.addEmpty(size);
	squish::CompressImage(pixels, width, height, comp, squish::kDxt1);

	// 1 COPY >_<
	dxtchk.tag = 'DXT1';
	str = dxtdata.take();
	dxtchk.maindata.resize(str.size());
	memcpy(dxtchk.maindata.data(), str.data(), str.size());
}

void ImportTexture(const std::filesystem::path& filepath, Chunk& chk, Chunk& dxtchk, int texid)
{
	int width, height, channels;
	uint8_t* pixels = stbi_load(filepath.u8string().c_str(), &width, &height, &channels, 4);
	std::string name = filepath.stem().u8string();
	ImportTexture(pixels, width, height, name, chk, dxtchk, texid);
	stbi_image_free(pixels);
}

void ImportTexture(const void* mem, size_t memSize, std::string_view name, Chunk& chk, Chunk& dxtchk, int texid)
{
	int width, height, channels;
	uint8_t* pixels = stbi_load_from_memory((const uint8_t*)mem, memSize, &width, &height, &channels, 4);
	ImportTexture(pixels, width, height, name, chk, dxtchk, texid);
	stbi_image_free(pixels);
}

DynArray<uint32_t> ConvertTextureToRGBA8(Chunk* texChunk) {
	const TexInfo* ti = (const TexInfo*)texChunk->maindata.data();
	uint8_t* firstbmp = texChunk->maindata.data() + 20;
	while (*(firstbmp++)); // skip name

	if (texChunk->tag == 'PALN')
	{
		uint32_t pal[256];
		uint8_t* pnt = firstbmp;
		for (int m = 0; m < (int)ti->numMipmaps; m++)
			pnt += *(uint32_t*)pnt + 4;
		uint32_t npalentries = *(uint32_t*)pnt; pnt += 4;
		if (npalentries > 256) npalentries = 256;
		memcpy(pal, pnt, 4 * npalentries);

		pnt = firstbmp;
		uint32_t mmsize = *(uint32_t*)pnt; pnt += 4;
		auto pix32 = DynArray<uint32_t>(mmsize);
		for (uint32_t p = 0; p < mmsize; p++)
			pix32[p] = pal[pnt[p]];
		return pix32;
	}
	else if (texChunk->tag == 'RGBA') {
		uint8_t* pnt = firstbmp;
		uint32_t mmsize = *(uint32_t*)pnt; pnt += 4;
		auto pix32 = DynArray<uint32_t>(mmsize/4);
		memcpy(pix32.data(), pnt, mmsize);
		return pix32;
	}
	return {};
}

void ExportTexture(Chunk* texChunk, const std::filesystem::path& filepath)
{
	const TexInfo* ti = (const TexInfo*)texChunk->maindata.data();
	auto rgba = ConvertTextureToRGBA8(texChunk);
	assert(rgba.size() > 0);
	stbi_write_png(filepath.u8string().c_str(), ti->width, ti->height, 4, rgba.data(), 0);
}

std::vector<uint8_t> ExportTextureToPNGInMemory(Chunk* texChunk)
{
	const TexInfo* ti = (const TexInfo*)texChunk->maindata.data();
	auto rgba = ConvertTextureToRGBA8(texChunk);
	assert(rgba.size() > 0);
	ByteWriter<std::vector<uint8_t>> byteWriter;
	auto writeFunc = [](void* context, void* data, int size) -> void {
		auto* bytes = (decltype(byteWriter)*)context;
		bytes->addData(data, size);
	};
	stbi_write_png_to_func(writeFunc, &byteWriter, ti->width, ti->height, 4, rgba.data(), 0);
	return byteWriter.take();
}

std::pair<Chunk*, Chunk*> FindTextureChunk(Scene& scene, uint32_t id)
{
	for (Chunk& chk : scene.palPack.subchunks) {
		uint32_t chkid = *(uint32_t*)chk.maindata.data();
		if (chkid == id) {
			int nth = &chk - scene.palPack.subchunks.data();
			assert(*(uint32_t*)scene.dxtPack.subchunks[nth].maindata.data() == id);
			return { &chk, &scene.dxtPack.subchunks[nth] };
		}
	}
	for (Chunk& chk : scene.lgtPack.subchunks) {
		uint32_t chkid = *(uint32_t*)chk.maindata.data();
		if (chkid == id) {
			return { &chk, nullptr };
		}
	}
	return { nullptr, nullptr };
}

std::pair<Chunk*, Chunk*> FindTextureChunkByName(Scene& scene, std::string_view name)
{
	for (Chunk& chk : scene.palPack.subchunks) {
		const TexInfo* ti = (const TexInfo*)chk.maindata.data();
		if (name == ti->name) {
			int nth = &chk - scene.palPack.subchunks.data();
			return { &chk, &scene.dxtPack.subchunks[nth] };
		}
	}
	return { nullptr, nullptr };
}

bool IsTextureUsingTransparency(const Chunk* texChunk)
{
	const TexInfo* ti = (const TexInfo*)texChunk->maindata.data();
	const uint8_t* firstbmp = texChunk->maindata.data() + 20;
	while (*(firstbmp++)); // skip name

	if (texChunk->tag == 'PALN')
	{
		uint32_t pal[256];
		const uint8_t* pnt = firstbmp;
		for (int m = 0; m < (int)ti->numMipmaps; m++)
			pnt += *(const uint32_t*)pnt + 4;
		uint32_t npalentries = *(const uint32_t*)pnt; pnt += 4;
		if (npalentries > 256)
			npalentries = 256;
		memcpy(pal, pnt, 4 * npalentries);

		pnt = firstbmp;
		const uint32_t mmsize = *(const uint32_t*)pnt; pnt += 4;
		for (uint32_t p = 0; p < mmsize; p++)
			if ((pal[pnt[p]] & 0xFF000000) != 0xFF000000)
				return true;
	}
	else if (texChunk->tag == 'RGBA') {
		const uint8_t* pnt = firstbmp;
		const uint32_t mmsize = *(const uint32_t*)pnt; pnt += 4;
		const uint32_t* pixels = reinterpret_cast<const uint32_t*>(pnt);
		for (size_t i = 0; i < mmsize / 4; ++i) {
			if ((pixels[i] & 0xFF000000) != 0xFF000000)
				return true;
		}
	}
	return false;
}
