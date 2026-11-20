/* ROM loading helpers: CRC32, a small inflate ("puff" by Mark Adler, public domain / zlib licence),
 * zip reader and folder scanner.  Taken from the author's Congo Bongo emulator. */
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace rz {

/* ===================================================================== */
/*  CRC32, inflate, zip                                                  */
/* ===================================================================== */
static uint32_t crc32(const uint8_t *p, size_t n)
{
	static uint32_t tab[256];
	if (!tab[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	}
	uint32_t c = 0xffffffffu;
	for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return ~c;
}

/* small inflate after Mark Adler's public-domain "puff" */
struct Inflater {
	const uint8_t *in; size_t inlen, incnt = 0;
	uint32_t bitbuf = 0; int bitcnt = 0;
	uint8_t *out; size_t outlen, outcnt = 0;
	bool err = false;

	struct Huff { uint16_t count[16]; uint16_t symbol[288]; };

	int bits(int need)
	{
		uint32_t val = bitbuf;
		while (bitcnt < need) {
			if (incnt >= inlen) { err = true; return 0; }
			val |= (uint32_t)in[incnt++] << bitcnt;
			bitcnt += 8;
		}
		bitbuf = val >> need;
		bitcnt -= need;
		return (int)(val & ((1u << need) - 1));
	}

	int decode(const Huff &h)
	{
		int code = 0, first = 0, index = 0;
		for (int len = 1; len <= 15; len++) {
			code |= bits(1);
			if (err) return -1;
			int count = h.count[len];
			if (code - count < first) return h.symbol[index + (code - first)];
			index += count; first += count; first <<= 1; code <<= 1;
		}
		return -1;
	}

	static int construct(Huff &h, const uint8_t *length, int n)
	{
		memset(h.count, 0, sizeof h.count);
		for (int s = 0; s < n; s++) h.count[length[s]]++;
		if (h.count[0] == n) return 0;
		int left = 1;
		for (int len = 1; len <= 15; len++) {
			left <<= 1; left -= h.count[len];
			if (left < 0) return left;
		}
		uint16_t offs[16]; offs[1] = 0;
		for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
		for (int s = 0; s < n; s++) if (length[s]) h.symbol[offs[length[s]]++] = (uint16_t)s;
		return left;
	}

	bool codes(const Huff &lc, const Huff &dc)
	{
		static const uint16_t lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
		static const uint16_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
		static const uint16_t dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
		static const uint16_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
		int sym;
		do {
			sym = decode(lc);
			if (sym < 0 || err) return false;
			if (sym < 256) {
				if (outcnt >= outlen) return false;
				out[outcnt++] = (uint8_t)sym;
			} else if (sym > 256) {
				sym -= 257;
				if (sym >= 29) return false;
				int len = lens[sym] + bits(lext[sym]);
				int ds = decode(dc);
				if (ds < 0 || ds >= 30) return false;
				size_t dist = dists[ds] + bits(dext[ds]);
				if (err || dist > outcnt || outcnt + len > outlen) return false;
				while (len--) { out[outcnt] = out[outcnt - dist]; outcnt++; }
			}
		} while (sym != 256);
		return true;
	}

	bool run()
	{
		int last;
		do {
			last = bits(1);
			int type = bits(2);
			if (err) return false;
			if (type == 0) {
				bitbuf = 0; bitcnt = 0;
				if (incnt + 4 > inlen) return false;
				unsigned len = in[incnt] | (in[incnt + 1] << 8);
				incnt += 4;
				if (incnt + len > inlen || outcnt + len > outlen) return false;
				memcpy(out + outcnt, in + incnt, len);
				incnt += len; outcnt += len;
			} else if (type == 1) {
				static Huff lc, dc; static bool init;
				if (!init) {
					uint8_t l[288];
					int s = 0;
					for (; s < 144; s++) l[s] = 8;
					for (; s < 256; s++) l[s] = 9;
					for (; s < 280; s++) l[s] = 7;
					for (; s < 288; s++) l[s] = 8;
					construct(lc, l, 288);
					for (s = 0; s < 30; s++) l[s] = 5;
					construct(dc, l, 30);
					init = true;
				}
				if (!codes(lc, dc)) return false;
			} else if (type == 2) {
				static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
				uint8_t lengths[320];
				int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
				if (err || nlen > 286 || ndist > 30) return false;
				int i = 0;
				for (; i < ncode; i++) lengths[order[i]] = (uint8_t)bits(3);
				for (; i < 19; i++) lengths[order[i]] = 0;
				Huff lc, dc;
				if (construct(lc, lengths, 19) != 0) return false;
				i = 0;
				while (i < nlen + ndist) {
					int sym = decode(lc);
					if (sym < 0 || err) return false;
					if (sym < 16) lengths[i++] = (uint8_t)sym;
					else {
						int len = 0, rep;
						if (sym == 16) { if (i == 0) return false; len = lengths[i - 1]; rep = 3 + bits(2); }
						else if (sym == 17) rep = 3 + bits(3);
						else rep = 11 + bits(7);
						if (i + rep > nlen + ndist) return false;
						while (rep--) lengths[i++] = (uint8_t)len;
					}
				}
				if (lengths[256] == 0) return false;
				if (construct(lc, lengths, nlen) < 0) return false;
				if (construct(dc, lengths + nlen, ndist) < 0) return false;
				if (!codes(lc, dc)) return false;
			} else return false;
		} while (!last);
		return true;
	}
};

struct RomFile { std::string name; std::vector<uint8_t> data; uint32_t crc; };

static bool read_all(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return false; }
	out.resize((size_t)n);
	bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16le(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void add_zip(const std::string &path, std::vector<RomFile> &out)
{
	std::vector<uint8_t> z;
	if (!read_all(path, z) || z.size() < 22) { fprintf(stderr, "cannot read %s\n", path.c_str()); return; }
	size_t eocd = std::string::npos;
	for (size_t i = z.size() - 22 + 1; i-- > 0;) {
		if (rd32(&z[i]) == 0x06054b50u) { eocd = i; break; }
		if (z.size() - i > 70000) break;
	}
	if (eocd == std::string::npos) { fprintf(stderr, "%s: not a zip file\n", path.c_str()); return; }
	unsigned n = rd16le(&z[eocd + 10]);
	size_t cd = rd32(&z[eocd + 16]);
	for (unsigned e = 0; e < n; e++) {
		if (cd + 46 > z.size() || rd32(&z[cd]) != 0x02014b50u) break;
		unsigned method = rd16le(&z[cd + 10]);
		uint32_t crc = rd32(&z[cd + 16]), csize = rd32(&z[cd + 20]), usize = rd32(&z[cd + 24]);
		unsigned nl = rd16le(&z[cd + 28]), xl = rd16le(&z[cd + 30]), cl = rd16le(&z[cd + 32]);
		size_t lho = rd32(&z[cd + 42]);
		std::string name((const char *)&z[cd + 46], nl);
		cd += 46 + nl + xl + cl;
		if (usize == 0 || usize > (4u << 20) || lho + 30 > z.size() || rd32(&z[lho]) != 0x04034b50u) continue;
		size_t ds = lho + 30 + rd16le(&z[lho + 26]) + rd16le(&z[lho + 28]);
		if (ds + csize > z.size()) continue;
		RomFile rf; rf.name = name; rf.data.resize(usize);
		if (method == 0 && csize == usize) memcpy(rf.data.data(), &z[ds], usize);
		else if (method == 8) {
			Inflater inf; inf.in = &z[ds]; inf.inlen = csize; inf.out = rf.data.data(); inf.outlen = usize;
			if (!inf.run() || inf.outcnt != usize) { fprintf(stderr, "%s: cannot unpack %s\n", path.c_str(), name.c_str()); continue; }
		} else continue;
		rf.crc = crc32(rf.data.data(), rf.data.size());
		size_t sl = rf.name.find_last_of("/\\");
		if (sl != std::string::npos) rf.name = rf.name.substr(sl + 1);
		out.push_back(std::move(rf));
	}
}

static bool ends_with_nocase(const std::string &s, const char *suf)
{
	size_t n = strlen(suf);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
	return true;
}

static void add_path(const std::string &path, std::vector<RomFile> &out)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (fs::is_directory(path, ec)) {
		for (auto &de : fs::directory_iterator(path, ec)) {
			if (!de.is_regular_file(ec)) continue;
			std::string p = de.path().string();
			if (ends_with_nocase(p, ".zip")) add_zip(p, out);
			else if (de.file_size(ec) <= (1u << 20)) {
				RomFile rf; rf.name = de.path().filename().string();
				if (read_all(p, rf.data) && !rf.data.empty()) { rf.crc = crc32(rf.data.data(), rf.data.size()); out.push_back(std::move(rf)); }
			}
		}
	} else if (ends_with_nocase(path, ".zip")) {
		add_zip(path, out);
	} else {
		RomFile rf; rf.name = fs::path(path).filename().string();
		if (read_all(path, rf.data) && !rf.data.empty()) { rf.crc = crc32(rf.data.data(), rf.data.size()); out.push_back(std::move(rf)); }
	}
}

/* ===================================================================== */

} // namespace rz
