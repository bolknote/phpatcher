/*
 * Shared helper for talking to the php normalizer coprocess (tools/normalize.php).
 *
 * One php process is spawned for the whole run; file contents are written in and
 * per-physical-line normalized keys come back. Used by both phpatcher-index (to
 * build a normalized corpus index) and phpatcher-match (to normalize the block
 * being factorized and the corpus files it extends across), so the two sides
 * agree byte-for-byte on what a "normalized line" is.
 *
 * Wire protocol (binary, little-endian u32 == PHP pack('V')):
 *   request:  u32 length, <length bytes of file content>
 *   response: u32 nlines, then per line: u8 flag, u32 klen, <klen bytes key>
 * flag: 0 = normal code (whitespace-collapsed), 1 = indivisible (raw bytes).
 */
#ifndef PHPATCHER_TOOLS_NORMALIZER_HPP
#define PHPATCHER_TOOLS_NORMALIZER_HPP

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>

namespace phpatcher {
namespace tools {

inline void put_u32le(std::string &buf, std::uint32_t v) {
    buf.push_back(static_cast<char>(v & 0xff));
    buf.push_back(static_cast<char>((v >> 8) & 0xff));
    buf.push_back(static_cast<char>((v >> 16) & 0xff));
    buf.push_back(static_cast<char>((v >> 24) & 0xff));
}

inline std::uint32_t get_u32le(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[noreturn]] inline void normalizer_fatal(const std::string &msg) {
    std::fprintf(stderr, "phpatcher: %s\n", msg.c_str());
    std::exit(2);
}

/* One normalized physical line. */
struct NormLine {
    std::uint8_t flag;   /* 0 = normal code, 1 = indivisible (raw bytes) */
    std::string key;
};

/* Persistent php normalizer coprocess. */
class Normalizer {
public:
    void start(const std::string &php, const std::string &script) {
        int to_child[2], from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0)
            normalizer_fatal("cannot create pipes for normalizer");

        pid_ = fork();
        if (pid_ < 0) normalizer_fatal("cannot fork normalizer");

        if (pid_ == 0) {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]); close(to_child[1]);
            close(from_child[0]); close(from_child[1]);
            execlp(php.c_str(), php.c_str(),
                   "-d", "display_errors=0", "-d", "error_reporting=0",
                   script.c_str(), static_cast<char *>(nullptr));
            std::fprintf(stderr, "phpatcher: cannot exec %s\n", php.c_str());
            _exit(127);
        }

        close(to_child[0]);
        close(from_child[1]);
        wr_ = to_child[1];
        rd_ = from_child[0];
    }

    ~Normalizer() {
        if (wr_ >= 0) close(wr_);  /* EOF -> coprocess shuts down */
        if (rd_ >= 0) close(rd_);
        if (pid_ > 0) {
            int status = 0;
            waitpid(pid_, &status, 0);
        }
    }

    bool started() const { return pid_ > 0; }

    /* Normalize one file's content into per-physical-line keys (index i -> the
     * 1-based physical line i+1). */
    std::vector<NormLine> run(std::string_view content) {
        std::string req;
        put_u32le(req, static_cast<std::uint32_t>(content.size()));
        req.append(content.data(), content.size());
        if (!write_all(req.data(), req.size()))
            normalizer_fatal("normalizer write failed (coprocess died?)");

        unsigned char hdr[4];
        if (!read_all(hdr, 4)) normalizer_fatal("normalizer read failed (coprocess died?)");
        const std::uint32_t nlines = get_u32le(hdr);

        std::vector<NormLine> out;
        out.reserve(nlines);
        for (std::uint32_t i = 0; i < nlines; ++i) {
            unsigned char meta[5];
            if (!read_all(meta, 5)) normalizer_fatal("normalizer truncated response");
            NormLine nl;
            nl.flag = meta[0];
            const std::uint32_t klen = get_u32le(meta + 1);
            nl.key.resize(klen);
            if (klen && !read_all(&nl.key[0], klen)) normalizer_fatal("normalizer truncated key");
            out.push_back(std::move(nl));
        }
        return out;
    }

private:
    bool write_all(const char *p, std::size_t n) {
        while (n > 0) {
            const ssize_t w = write(wr_, p, n);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            if (w == 0) return false;
            p += w; n -= static_cast<std::size_t>(w);
        }
        return true;
    }
    bool read_all(void *buf, std::size_t n) {
        auto *p = static_cast<char *>(buf);
        while (n > 0) {
            const ssize_t r = read(rd_, p, n);
            if (r < 0) { if (errno == EINTR) continue; return false; }
            if (r == 0) return false;  /* EOF: coprocess gone */
            p += r; n -= static_cast<std::size_t>(r);
        }
        return true;
    }

    pid_t pid_ = -1;
    int wr_ = -1;
    int rd_ = -1;
};

}  // namespace tools
}  // namespace phpatcher

#endif  // PHPATCHER_TOOLS_NORMALIZER_HPP
