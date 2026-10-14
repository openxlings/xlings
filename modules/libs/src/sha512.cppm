// xlings.libs.sha512 -- in-process SHA-512 (FIPS 180-4) and the SHA-512
// password hash ("$6$", Ulrich Drepper's SHA-crypt specification) that
// /etc/shadow, login, su and sudo read on every libc a Luban system has.
//
// In-process, like sha256: a user's password is hashed where it was typed,
// never handed to an external program (an argv is readable by any process,
// and the tool would be a host dependency besides).

export module xlings.libs.sha512;

import std;

export namespace xlings::sha512 {

class Hasher {
public:
    Hasher();
    void reset();
    void update(const void* data, std::size_t len);
    // The 64-byte digest. The context is consumed; reset() to reuse.
    std::array<unsigned char, 64> digest();

private:
    void compress_(const unsigned char* block);
    std::array<std::uint64_t, 8> h_{};
    unsigned char buf_[128]{};
    std::size_t buflen_ = 0;
    std::uint64_t total_ = 0;
};

std::string hex(std::string_view data);

// crypt(3)'s "$6$" for `password` with `salt` (at most 16 characters are
// used) and `rounds` (5000 is the default and then not written).
std::string crypt(std::string_view password, std::string_view salt, unsigned rounds = 5000);

// 16 salt characters from the system's randomness, in crypt's alphabet.
std::string random_salt();

}  // namespace xlings::sha512
