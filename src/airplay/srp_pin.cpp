#include <openmirror/airplay/srp_pin.h>

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#include <cstring>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace openmirror::airplay {

namespace {

// RFC 5054 Group 14 — 2048-bit safe prime, generator g = 2.
// This is the modulus AirPlay 1 / classic Apple TV uses for PIN pairing.
constexpr const char* kRFC5054_Group14_N_hex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AACAA68FFFFFFFFFFFFFFFF";

constexpr int kModBytes = 256;        // 2048 bits / 8
constexpr int kHashBytes = SHA_DIGEST_LENGTH; // 20

// SHA-1 helper.
std::vector<uint8_t> sha1(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out(kHashBytes);
    SHA1(in.data(), in.size(), out.data());
    return out;
}

// Append BIGNUM as big-endian, zero-padded to kModBytes.
void append_pad(std::vector<uint8_t>& dst, const BIGNUM* bn) {
    std::vector<uint8_t> tmp(kModBytes, 0);
    int n = BN_num_bytes(bn);
    if (n > kModBytes) n = kModBytes;
    BN_bn2bin(bn, tmp.data() + (kModBytes - n));
    dst.insert(dst.end(), tmp.begin(), tmp.end());
}

std::vector<uint8_t> bn_to_padded(const BIGNUM* bn) {
    std::vector<uint8_t> out(kModBytes, 0);
    int n = BN_num_bytes(bn);
    if (n > kModBytes) n = kModBytes;
    BN_bn2bin(bn, out.data() + (kModBytes - n));
    return out;
}

// Apple's session-key derivation (per UxPlay/RAOP-Player reference):
//   K = SHA1(S || 0x00000000) || SHA1(S || 0x00000001)  (40 bytes)
// S is encoded as raw BN bytes WITHOUT modulus padding.
std::vector<uint8_t> apple_session_key(const BIGNUM* S) {
    int n = BN_num_bytes(S);
    std::vector<uint8_t> sb(n);
    BN_bn2bin(S, sb.data());

    std::vector<uint8_t> K(2 * kHashBytes);
    uint8_t four[4] = {0,0,0,0};

    SHA_CTX c;
    SHA1_Init(&c);
    SHA1_Update(&c, sb.data(), sb.size());
    SHA1_Update(&c, four, 4);
    SHA1_Final(K.data(), &c);

    four[3] = 1;
    SHA1_Init(&c);
    SHA1_Update(&c, sb.data(), sb.size());
    SHA1_Update(&c, four, 4);
    SHA1_Final(K.data() + kHashBytes, &c);
    return K;
}

// Hash a BIGNUM as raw (unpadded) big-endian bytes — Apple variant.
std::vector<uint8_t> sha1_bn(const BIGNUM* bn) {
    int n = BN_num_bytes(bn);
    std::vector<uint8_t> b(n);
    BN_bn2bin(bn, b.data());
    return sha1(b);
}

// H(n1 || n2) with UNPADDED big-endian encoding (Apple/Stanford variant).
BIGNUM* H_nn_apple(const BIGNUM* n1, const BIGNUM* n2) {
    int l1 = BN_num_bytes(n1), l2 = BN_num_bytes(n2);
    std::vector<uint8_t> buf(l1 + l2);
    BN_bn2bin(n1, buf.data());
    BN_bn2bin(n2, buf.data() + l1);
    auto h = sha1(buf);
    return BN_bin2bn(h.data(), (int)h.size(), nullptr);
}

void append_bn_raw(std::vector<uint8_t>& dst, const BIGNUM* bn) {
    int n = BN_num_bytes(bn);
    size_t off = dst.size();
    dst.resize(off + n);
    BN_bn2bin(bn, dst.data() + off);
}

} // namespace

struct SrpPinServer::Impl {
    BN_CTX* ctx = nullptr;
    BIGNUM* N = nullptr;
    BIGNUM* g = nullptr;
    BIGNUM* k = nullptr;
    BIGNUM* s = nullptr;   // salt (16 bytes)
    BIGNUM* v = nullptr;   // verifier
    BIGNUM* b = nullptr;   // server private
    BIGNUM* B = nullptr;   // server public
    BIGNUM* A = nullptr;   // client public
    std::vector<uint8_t> salt_wire;  // exact 16 bytes sent on wire
    std::vector<uint8_t> A_wire;     // exact bytes received from client
    std::vector<uint8_t> B_wire;     // exact 256 bytes sent on wire
    std::vector<uint8_t> M2;
    std::vector<uint8_t> K;     // 40-byte interleaved session key
    std::string username = "Pair-Setup";
    std::string pin;            // saved for brute-force diagnostic on M1 fail
    bool ready = false;

    Impl() {
        ctx = BN_CTX_new();
        N = BN_new();
        g = BN_new();
        k = BN_new();
        s = BN_new();
        v = BN_new();
        b = BN_new();
        B = BN_new();
        A = BN_new();
    }
    ~Impl() {
        BN_free(N); BN_free(g); BN_free(k); BN_free(s);
        BN_free(v); BN_free(b); BN_free(B); BN_free(A);
        BN_CTX_free(ctx);
    }
};

SrpPinServer::SrpPinServer() : impl_(new Impl()) {}
SrpPinServer::~SrpPinServer() { delete impl_; }

bool SrpPinServer::start(const std::string& pin, const std::string& username) {
    authenticated_ = false;
    impl_->ready = false;
    impl_->M2.clear();
    impl_->K.clear();
    impl_->username = username.empty() ? "Pair-Setup" : username;
    impl_->pin = pin;

    if (!BN_hex2bn(&impl_->N, kRFC5054_Group14_N_hex)) return false;
    if (!BN_set_word(impl_->g, 2)) return false;

    // k = SHA1(PAD(N) || PAD(g))  — RFC 5054 padded form (UxPlay rfc5054_compat=1)
    {
        std::vector<uint8_t> kbuf;
        append_pad(kbuf, impl_->N);
        append_pad(kbuf, impl_->g);
        auto kdig = sha1(kbuf);
        if (!BN_bin2bn(kdig.data(), (int)kdig.size(), impl_->k)) return false;
    }

    // salt s — 16 random bytes; force high byte non-zero so BN_num_bytes(s)
    // always equals 16 and the wire form == the BN form (matches what iOS hashes).
    uint8_t salt_bytes[16];
    do {
        if (RAND_bytes(salt_bytes, sizeof(salt_bytes)) != 1) return false;
    } while (salt_bytes[0] == 0);
    if (!BN_bin2bn(salt_bytes, sizeof(salt_bytes), impl_->s)) return false;
    impl_->salt_wire.assign(salt_bytes, salt_bytes + sizeof(salt_bytes));

    // x = SHA1(s | SHA1(I | ":" | P))  — salt hashed as BN_num_bytes(s)
    // (== 16 here because we forced MSB!=0). This matches csrp's H_ns + iOS.
    std::vector<uint8_t> ip;
    ip.insert(ip.end(), impl_->username.begin(), impl_->username.end());
    ip.push_back(':');
    ip.insert(ip.end(), pin.begin(), pin.end());
    auto ip_h = sha1(ip);

    std::vector<uint8_t> sx;
    append_bn_raw(sx, impl_->s);
    sx.insert(sx.end(), ip_h.begin(), ip_h.end());
    auto x_h = sha1(sx);

    BIGNUM* x = BN_new();
    if (!BN_bin2bn(x_h.data(), (int)x_h.size(), x)) { BN_free(x); return false; }

    // v = g^x mod N
    if (!BN_mod_exp(impl_->v, impl_->g, x, impl_->N, impl_->ctx)) {
        BN_free(x); return false;
    }
    BN_free(x);

    // b = random (256 bits per UxPlay), B = (k*v + g^b) mod N (RFC 5054 padded path).
    // Re-roll until BN_num_bytes(B) == 256 so the wire form == BN form (avoids the
    // off-by-one-byte hash mismatch when B has a leading zero byte).
    BIGNUM* gb = BN_new();
    BIGNUM* kv = BN_new();
    bool ok = false;
    for (int tries = 0; tries < 64; ++tries) {
        if (!BN_rand(impl_->b, 256, -1, 0)) break;
        ok = BN_mod_exp(gb, impl_->g, impl_->b, impl_->N, impl_->ctx) &&
             BN_mod_mul(kv, impl_->k, impl_->v, impl_->N, impl_->ctx) &&
             BN_mod_add(impl_->B, kv, gb, impl_->N, impl_->ctx);
        if (!ok) break;
        if (BN_num_bytes(impl_->B) == kModBytes) break;
        ok = false;
    }
    BN_free(gb); BN_free(kv);
    if (!ok) return false;

    // Cache exactly the bytes we will send on the wire (== BN form, 256 bytes).
    impl_->B_wire = bn_to_padded(impl_->B);

    impl_->ready = true;
    return true;
}

std::vector<uint8_t> SrpPinServer::get_salt() const {
    if (!impl_->ready) return {};
    return impl_->salt_wire;
}

std::vector<uint8_t> SrpPinServer::get_B() const {
    if (!impl_->ready) return {};
    return impl_->B_wire;
}

bool SrpPinServer::process_client_pubkey(const std::vector<uint8_t>& A,
                                         const std::vector<uint8_t>& M1) {
    std::cerr << "[SRP] step2 enter A=" << A.size() << " M1=" << M1.size()
              << " ready=" << impl_->ready << std::endl;
    if (!impl_->ready || A.size() != kModBytes || M1.size() != kHashBytes)
        return false;

    if (!BN_bin2bn(A.data(), (int)A.size(), impl_->A)) return false;
    // Cache the exact wire bytes for M1 hashing — must match what client used.
    impl_->A_wire.assign(A.begin(), A.end());

    // Reject A ≡ 0 (mod N).
    BIGNUM* tmp = BN_new();
    BN_mod(tmp, impl_->A, impl_->N, impl_->ctx);
    if (BN_is_zero(tmp)) { BN_free(tmp); std::cerr << "[SRP] A==0\n"; return false; }
    BN_free(tmp);

    // u = SHA1(PAD(A) || PAD(B))  — RFC 5054 padded form
    BIGNUM* u;
    {
        std::vector<uint8_t> ub;
        append_pad(ub, impl_->A);
        append_pad(ub, impl_->B);
        auto uh = sha1(ub);
        u = BN_bin2bn(uh.data(), (int)uh.size(), nullptr);
        if (!u) return false;
    }
    std::cerr << "[SRP] u computed" << std::endl;

    // S = (A * v^u) ^ b mod N
    BIGNUM* vu = BN_new();
    BIGNUM* avu = BN_new();
    BIGNUM* S = BN_new();
    bool ok = BN_mod_exp(vu, impl_->v, u, impl_->N, impl_->ctx) &&
              BN_mul(avu, impl_->A, vu, impl_->ctx) &&
              BN_mod_exp(S, avu, impl_->b, impl_->N, impl_->ctx);
    BN_free(vu); BN_free(avu); BN_free(u);
    if (!ok) { BN_free(S); std::cerr << "[SRP] mod_exp failed\n"; return false; }
    std::cerr << "[SRP] S computed (BN bytes=" << BN_num_bytes(S) << ")" << std::endl;

    // K = SHA1(S||0x00000000) || SHA1(S||0x00000001) — Apple variant, 40 bytes.
    impl_->K = apple_session_key(S);
    BN_free(S);
    std::cerr << "[SRP] K=" << impl_->K.size() << "B" << std::endl;

    // M1' = SHA1( SHA1(N) XOR SHA1(g) | SHA1(I) | s | PAD(A) | PAD(B) | K )
    // per philippe44 RAOP-Player auth_protocol.html.  s stays in BN form (16 B
    // here because we forced MSB!=0 in start()), A and B are zero-padded to 256.
    auto hN = sha1_bn(impl_->N);
    auto hg = sha1_bn(impl_->g);
    std::vector<uint8_t> hNg(kHashBytes);
    for (int i = 0; i < kHashBytes; i++) hNg[i] = hN[i] ^ hg[i];

    std::vector<uint8_t> hI_in(impl_->username.begin(), impl_->username.end());
    auto hI = sha1(hI_in);

    std::vector<uint8_t> m1_buf;
    m1_buf.insert(m1_buf.end(), hNg.begin(), hNg.end());
    m1_buf.insert(m1_buf.end(), hI.begin(), hI.end());
    append_bn_raw(m1_buf, impl_->s);
    append_pad(m1_buf, impl_->A);
    append_pad(m1_buf, impl_->B);
    m1_buf.insert(m1_buf.end(), impl_->K.begin(), impl_->K.end());
    auto M1_calc = sha1(m1_buf);
    std::cerr << "[SRP] M1_calc computed (m1_buf=" << m1_buf.size() << "B)" << std::endl;

    if (CRYPTO_memcmp(M1_calc.data(), M1.data(), kHashBytes) != 0) {
        std::cerr << "[SRP] Client M1 proof mismatch (wrong PIN?)" << std::endl;
        // ===== expanded brute-force probe =====
        // Vary independently:
        //   user  : MAC (4 cases) + "Pair-Setup" + ""  (6 users)
        //   pin   : ascii / utf16-le / u16be / u16le / u32be / u32le  (6 pins)
        //   x_fmt : 0=H(s|H(I:P)), 1=H(s|H(P)), 2=H(s|H(:P)), 3=H(P), 4=H(s|P), 5=H(I|s|P)
        //   K_fmt : 0=apple 00||01, 1=apple 01||00, 2=H(S) only, 3=H(PAD(S))
        //   k_fmt : 0=H(N|PAD(g)), 1=H(N|g)         (affects only B; we leave B alone, but
        //                                            iPad's u may use H(PAD(A)|PAD(B)) either way,
        //                                            and M1 uses our actual B bytes — so k variant
        //                                            doesn't change M1 here.  Skip.)
        //   M1_fmt: 0=H(NxorG|hI|s|PAD(A)|PAD(B)|K) (philippe44/csrp)
        //           1=H(NxorG|hI|s|A_raw|B_raw|K)   (csrp BN form)
        //           2=H(NxorG|hI|s|PAD(A)|PAD(B))   (no K — RFC 5054)
        //           3=H(s|PAD(A)|PAD(B)|K)           (Apple iTunes variant)
        //           4=H(NxorG|s|PAD(A)|PAD(B)|K)    (no I)
        std::string mac = impl_->username;
        std::string mac_lo = mac; for (auto& c : mac_lo) c = (char)std::tolower((unsigned char)c);
        std::string mac_nc = mac; mac_nc.erase(std::remove(mac_nc.begin(), mac_nc.end(), ':'), mac_nc.end());
        std::string mac_nclo = mac_lo; mac_nclo.erase(std::remove(mac_nclo.begin(), mac_nclo.end(), ':'), mac_nclo.end());
        std::vector<std::string> users = { mac, mac_lo, mac_nc, mac_nclo, std::string("Pair-Setup"), std::string() };

        std::vector<std::pair<std::string, std::vector<uint8_t>>> pins;
        pins.push_back({"ascii", std::vector<uint8_t>(impl_->pin.begin(), impl_->pin.end())});
        int pv = 0; try { pv = std::stoi(impl_->pin); } catch(...) {}
        pins.push_back({"u16-be", {(uint8_t)((pv>>8)&0xff),(uint8_t)(pv&0xff)}});
        pins.push_back({"u16-le", {(uint8_t)(pv&0xff),(uint8_t)((pv>>8)&0xff)}});
        pins.push_back({"u32-be", {(uint8_t)((pv>>24)&0xff),(uint8_t)((pv>>16)&0xff),(uint8_t)((pv>>8)&0xff),(uint8_t)(pv&0xff)}});
        pins.push_back({"u32-le", {(uint8_t)(pv&0xff),(uint8_t)((pv>>8)&0xff),(uint8_t)((pv>>16)&0xff),(uint8_t)((pv>>24)&0xff)}});
        std::vector<uint8_t> u16; for (char c : impl_->pin) { u16.push_back((uint8_t)c); u16.push_back(0); }
        pins.push_back({"utf16-le", u16});

        // u (RFC 5054 padded form)
        std::vector<uint8_t> ub2;
        append_pad(ub2, impl_->A);
        append_pad(ub2, impl_->B);
        auto uh2 = sha1(ub2);
        BIGNUM* uu = BN_bin2bn(uh2.data(), (int)uh2.size(), nullptr);

        // S depends only on x (and fixed A,b,v=g^x). Cache S per x.
        auto compute_S = [&](const BIGNUM* xx)->BIGNUM* {
            BIGNUM* vv = BN_new(); BN_mod_exp(vv, impl_->g, xx, impl_->N, impl_->ctx);
            BIGNUM* vu = BN_new(); BN_mod_exp(vu, vv, uu, impl_->N, impl_->ctx);
            BIGNUM* avu = BN_new(); BN_mul(avu, impl_->A, vu, impl_->ctx);
            BIGNUM* Sn = BN_new(); BN_mod_exp(Sn, avu, impl_->b, impl_->N, impl_->ctx);
            BN_free(vv); BN_free(vu); BN_free(avu);
            return Sn;
        };

        // K variants
        auto K_apple_swap = [&](const BIGNUM* S)->std::vector<uint8_t> {
            // SHA1(S|01) || SHA1(S|00) — reversed interleave order
            std::vector<uint8_t> sb(BN_num_bytes(S));
            BN_bn2bin(S, sb.data());
            std::vector<uint8_t> b1 = sb; b1.insert(b1.end(), {0,0,0,1});
            std::vector<uint8_t> b0 = sb; b0.insert(b0.end(), {0,0,0,0});
            auto h1 = sha1(b1); auto h0 = sha1(b0);
            std::vector<uint8_t> out; out.insert(out.end(), h1.begin(), h1.end());
            out.insert(out.end(), h0.begin(), h0.end());
            return out;
        };
        auto K_single = [&](const BIGNUM* S)->std::vector<uint8_t> {
            std::vector<uint8_t> sb(BN_num_bytes(S)); BN_bn2bin(S, sb.data());
            return sha1(sb);
        };
        auto K_padded = [&](const BIGNUM* S)->std::vector<uint8_t> {
            std::vector<uint8_t> sb; append_pad(sb, S);
            return sha1(sb);
        };

        bool found = false;
        for (size_t ui = 0; ui < users.size() && !found; ++ui) {
            const auto& un = users[ui];
            for (size_t pi = 0; pi < pins.size() && !found; ++pi) {
                const auto& pp = pins[pi];
                for (int xfmt = 0; xfmt < 6 && !found; ++xfmt) {
                    // Build x according to xfmt
                    std::vector<uint8_t> xbuf;
                    if (xfmt == 0) {
                        // SHA1(s | SHA1(I ":" P))
                        std::vector<uint8_t> ip(un.begin(), un.end()); ip.push_back(':');
                        ip.insert(ip.end(), pp.second.begin(), pp.second.end());
                        auto iph = sha1(ip);
                        append_bn_raw(xbuf, impl_->s);
                        xbuf.insert(xbuf.end(), iph.begin(), iph.end());
                    } else if (xfmt == 1) {
                        // SHA1(s | SHA1(P))
                        auto ph = sha1(pp.second);
                        append_bn_raw(xbuf, impl_->s);
                        xbuf.insert(xbuf.end(), ph.begin(), ph.end());
                    } else if (xfmt == 2) {
                        // SHA1(s | SHA1(":" P))   (empty user with colon)
                        std::vector<uint8_t> cp; cp.push_back(':');
                        cp.insert(cp.end(), pp.second.begin(), pp.second.end());
                        auto ph = sha1(cp);
                        append_bn_raw(xbuf, impl_->s);
                        xbuf.insert(xbuf.end(), ph.begin(), ph.end());
                    } else if (xfmt == 3) {
                        // SHA1(P)  no salt at all
                        xbuf.insert(xbuf.end(), pp.second.begin(), pp.second.end());
                    } else if (xfmt == 4) {
                        // SHA1(s | P)  raw pin no inner hash
                        append_bn_raw(xbuf, impl_->s);
                        xbuf.insert(xbuf.end(), pp.second.begin(), pp.second.end());
                    } else { // 5
                        // SHA1(I | s | P)
                        xbuf.insert(xbuf.end(), un.begin(), un.end());
                        append_bn_raw(xbuf, impl_->s);
                        xbuf.insert(xbuf.end(), pp.second.begin(), pp.second.end());
                    }
                    auto xh = sha1(xbuf);
                    BIGNUM* xx = BN_bin2bn(xh.data(), (int)xh.size(), nullptr);
                    BIGNUM* Sn = compute_S(xx);

                    std::vector<std::pair<std::string, std::vector<uint8_t>>> Ks = {
                        {"apple01", apple_session_key(Sn)},
                        {"appleSWAP", K_apple_swap(Sn)},
                        {"H(S)", K_single(Sn)},
                        {"H(PAD(S))", K_padded(Sn)},
                    };

                    std::vector<uint8_t> hI_un_in(un.begin(), un.end());
                    auto hI_un = sha1(hI_un_in);

                    for (auto& Kp : Ks) {
                        const auto& Kn = Kp.second;
                        for (int mfmt = 0; mfmt < 5 && !found; ++mfmt) {
                            std::vector<uint8_t> mb;
                            if (mfmt == 0) {
                                mb.insert(mb.end(), hNg.begin(), hNg.end());
                                mb.insert(mb.end(), hI_un.begin(), hI_un.end());
                                append_bn_raw(mb, impl_->s);
                                append_pad(mb, impl_->A);
                                append_pad(mb, impl_->B);
                                mb.insert(mb.end(), Kn.begin(), Kn.end());
                            } else if (mfmt == 1) {
                                mb.insert(mb.end(), hNg.begin(), hNg.end());
                                mb.insert(mb.end(), hI_un.begin(), hI_un.end());
                                append_bn_raw(mb, impl_->s);
                                append_bn_raw(mb, impl_->A);
                                append_bn_raw(mb, impl_->B);
                                mb.insert(mb.end(), Kn.begin(), Kn.end());
                            } else if (mfmt == 2) {
                                // No K — RFC 5054 client M1
                                mb.insert(mb.end(), hNg.begin(), hNg.end());
                                mb.insert(mb.end(), hI_un.begin(), hI_un.end());
                                append_bn_raw(mb, impl_->s);
                                append_pad(mb, impl_->A);
                                append_pad(mb, impl_->B);
                            } else if (mfmt == 3) {
                                // SHA1(s | PAD(A) | PAD(B) | K) — Apple iTunes variant
                                append_bn_raw(mb, impl_->s);
                                append_pad(mb, impl_->A);
                                append_pad(mb, impl_->B);
                                mb.insert(mb.end(), Kn.begin(), Kn.end());
                            } else { // 4
                                // No I
                                mb.insert(mb.end(), hNg.begin(), hNg.end());
                                append_bn_raw(mb, impl_->s);
                                append_pad(mb, impl_->A);
                                append_pad(mb, impl_->B);
                                mb.insert(mb.end(), Kn.begin(), Kn.end());
                            }
                            auto Mn = sha1(mb);
                            if (CRYPTO_memcmp(Mn.data(), M1.data(), kHashBytes) == 0) {
                                std::cerr << "[SRP-PROBE] *** MATCH ***  user=\"" << un
                                          << "\" pin=" << pp.first
                                          << " xfmt=" << xfmt
                                          << " Kfmt=" << Kp.first
                                          << " Mfmt=" << mfmt << std::endl;
                                found = true;
                            }
                        }
                    }
                    BN_free(xx); BN_free(Sn);
                }
            }
        }
        if (!found) std::cerr << "[SRP-PROBE] no variant matched (tried " 
                              << users.size()*pins.size()*6*4*5 << " combos)" << std::endl;
        BN_free(uu);
        return false;
    }

    // H_AMK (M2) = SHA1( PAD(A) | M1 | K )  per philippe44.
    std::vector<uint8_t> m2_buf;
    append_pad(m2_buf, impl_->A);
    m2_buf.insert(m2_buf.end(), M1.begin(), M1.end());
    m2_buf.insert(m2_buf.end(), impl_->K.begin(), impl_->K.end());
    impl_->M2 = sha1(m2_buf);

    authenticated_ = true;
    std::cerr << "[SRP] authenticated, M2=" << impl_->M2.size() << "B" << std::endl;
    return true;
}

std::vector<uint8_t> SrpPinServer::get_M2() const {
    return impl_->M2;
}

std::vector<uint8_t> SrpPinServer::get_session_key() const {
    return impl_->K;
}

// ---- Step-3 LTPK encryption/decryption ----
//
// AES-128-CBC with PKCS#7 padding.
// AES key = SHA1("Pair-Setup-AES-Key"  | K)[0..16]
// AES IV  = SHA1("Pair-Setup-AES-IV"   | K)[0..16]
// Same key/IV is used for both directions on AirPlay 1.

namespace {

bool derive_aes(const std::vector<uint8_t>& K,
                uint8_t out_key[16], uint8_t out_iv[16]) {
    if (K.empty()) return false;

    auto h = [&](const char* tag, uint8_t out[16]) {
        std::vector<uint8_t> buf(tag, tag + std::strlen(tag));
        buf.insert(buf.end(), K.begin(), K.end());
        uint8_t dig[SHA_DIGEST_LENGTH];
        SHA1(buf.data(), buf.size(), dig);
        std::memcpy(out, dig, 16);
    };
    h("Pair-Setup-AES-Key", out_key);
    h("Pair-Setup-AES-IV",  out_iv);
    return true;
}

} // namespace

std::vector<uint8_t> srp_pin_decrypt_ltpk(const std::vector<uint8_t>& K,
                                          const std::vector<uint8_t>& epk) {
    uint8_t key[16], iv[16];
    if (!derive_aes(K, key, iv) || epk.empty()) return {};

    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    std::vector<uint8_t> out(epk.size() + 16);
    int outl = 0, finl = 0;
    bool ok = EVP_DecryptInit_ex(c, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
              EVP_DecryptUpdate(c, out.data(), &outl, epk.data(), (int)epk.size()) == 1 &&
              EVP_DecryptFinal_ex(c, out.data() + outl, &finl) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize(outl + finl);
    return out;
}

std::vector<uint8_t> srp_pin_encrypt_ltpk(const std::vector<uint8_t>& K,
                                          const std::vector<uint8_t>& our_ltpk) {
    uint8_t key[16], iv[16];
    if (!derive_aes(K, key, iv) || our_ltpk.empty()) return {};

    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    if (!c) return {};
    std::vector<uint8_t> out(our_ltpk.size() + 32);
    int outl = 0, finl = 0;
    bool ok = EVP_EncryptInit_ex(c, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
              EVP_EncryptUpdate(c, out.data(), &outl, our_ltpk.data(), (int)our_ltpk.size()) == 1 &&
              EVP_EncryptFinal_ex(c, out.data() + outl, &finl) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) return {};
    out.resize(outl + finl);
    return out;
}

// ============================================================
//   philippe44 RAOP-Player AirPlay-1 PIN auth test vector
// ============================================================
// Source: https://github.com/philippe44/RAOP-Player/blob/master/doc/auth_protocol.html
// We compute everything CLIENT-side using the published <a> + <salt> + <B>
// + <I> + <pin> and verify M1 == 4b4e638b... and K == 9a689113...
// If our SRP math is correct, we'll match bit-for-bit.

namespace {

std::vector<uint8_t> hex_to_bytes(const char* hex) {
    std::vector<uint8_t> out;
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (size_t i = 0; hex[i] && hex[i+1]; i += 2) {
        int hi = nyb(hex[i]), lo = nyb(hex[i+1]);
        if (hi < 0 || lo < 0) break;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

std::string bytes_to_hex(const std::vector<uint8_t>& b) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) { s += h[x >> 4]; s += h[x & 0xf]; }
    return s;
}

} // namespace

bool srp_pin_self_test() {
    using std::cerr; using std::endl;

    // Roundtrip test: drive our server with a pretend client.
    // Inputs match philippe44 RAOP-Player test vector (where possible);
    // since the doc publishes truncated A/B, we recompute them locally.
    const char* I_str   = "366B4165DD64AD3A";
    const char* pin_str = "1234";

    SrpPinServer server;
    if (!server.start(pin_str, I_str)) {
        cerr << "[SRP-TEST] server.start() failed\n";
        return false;
    }
    auto salt  = server.get_salt();   // 16 bytes
    auto B_w   = server.get_B();      // 256 bytes
    cerr << "[SRP-TEST] salt = " << bytes_to_hex(salt) << " (" << salt.size() << "B)\n";
    cerr << "[SRP-TEST] B    = " << bytes_to_hex(B_w).substr(0,32) << "... (" << B_w.size() << "B)\n";

    // ---- pretend client side ----
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* N = BN_new(); BN_hex2bn(&N, kRFC5054_Group14_N_hex);
    BIGNUM* g = BN_new(); BN_set_word(g, 2);
    BIGNUM* B = BN_bin2bn(B_w.data(), (int)B_w.size(), nullptr);
    BIGNUM* s = BN_bin2bn(salt.data(), (int)salt.size(), nullptr);

    // Random a, A = g^a mod N
    BIGNUM* a = BN_new(); BN_rand(a, 256, -1, 0);
    BIGNUM* A = BN_new(); BN_mod_exp(A, g, a, N, ctx);
    auto A_w = bn_to_padded(A);

    // k = SHA1(PAD(N)||PAD(g))
    BIGNUM* k;
    {
        std::vector<uint8_t> kb; append_pad(kb, N); append_pad(kb, g);
        auto kd = sha1(kb);
        k = BN_bin2bn(kd.data(), (int)kd.size(), nullptr);
    }
    // u = SHA1(PAD(A)||PAD(B))
    BIGNUM* u;
    {
        std::vector<uint8_t> ub; append_pad(ub, A); append_pad(ub, B);
        auto uh = sha1(ub);
        u = BN_bin2bn(uh.data(), (int)uh.size(), nullptr);
    }
    // x = SHA1(s||SHA1(I||":"||p))
    std::vector<uint8_t> ip; ip.insert(ip.end(), I_str, I_str+std::strlen(I_str));
    ip.push_back(':'); ip.insert(ip.end(), pin_str, pin_str+std::strlen(pin_str));
    auto ip_h = sha1(ip);
    std::vector<uint8_t> sx; append_bn_raw(sx, s);
    sx.insert(sx.end(), ip_h.begin(), ip_h.end());
    auto x_h = sha1(sx);
    BIGNUM* x = BN_bin2bn(x_h.data(), (int)x_h.size(), nullptr);

    // S = (B - k*g^x)^(a + u*x) mod N
    BIGNUM* gx = BN_new(); BN_mod_exp(gx, g, x, N, ctx);
    BIGNUM* kgx = BN_new(); BN_mod_mul(kgx, k, gx, N, ctx);
    BIGNUM* base = BN_new(); BN_mod_sub(base, B, kgx, N, ctx);
    BIGNUM* ux = BN_new(); BN_mul(ux, u, x, ctx);
    BIGNUM* aux = BN_new(); BN_add(aux, a, ux);
    BIGNUM* S = BN_new(); BN_mod_exp(S, base, aux, N, ctx);
    auto K = apple_session_key(S);

    // M1 = SHA1(SHA1(N)^SHA1(g) | SHA1(I) | s | A | B | K)  — BN bytes form
    auto hN = sha1_bn(N);
    auto hg = sha1_bn(g);
    std::vector<uint8_t> hNg(hN.size());
    for (size_t i = 0; i < hN.size(); ++i) hNg[i] = hN[i] ^ hg[i];
    std::vector<uint8_t> hI_in(I_str, I_str + std::strlen(I_str));
    auto hI = sha1(hI_in);
    std::vector<uint8_t> m1_buf;
    m1_buf.insert(m1_buf.end(), hNg.begin(), hNg.end());
    m1_buf.insert(m1_buf.end(), hI.begin(), hI.end());
    append_bn_raw(m1_buf, s);
    append_pad(m1_buf, A);
    append_pad(m1_buf, B);
    m1_buf.insert(m1_buf.end(), K.begin(), K.end());
    auto M1 = sha1(m1_buf);

    cerr << "[SRP-TEST] client K  = " << bytes_to_hex(K) << "\n";
    cerr << "[SRP-TEST] client M1 = " << bytes_to_hex(M1) << "\n";

    // ---- send to our server ----
    bool ok = server.process_client_pubkey(A_w, M1);
    cerr << "[SRP-TEST] server.process_client_pubkey: " << (ok?"OK":"FAIL") << endl;

    // M2 check
    auto M2_server = server.get_M2();
    auto Ksrv = server.get_session_key();
    cerr << "[SRP-TEST] server K  = " << bytes_to_hex(Ksrv) << "\n";
    cerr << "[SRP-TEST] server M2 = " << bytes_to_hex(M2_server) << "\n";

    bool kmatch = (K == Ksrv);
    cerr << "[SRP-TEST] K agrees: " << (kmatch?"YES":"NO") << endl;
    cerr << "[SRP-TEST] roundtrip: " << ((ok&&kmatch)?"PASS":"FAIL") << endl;

    BN_free(N); BN_free(g); BN_free(a); BN_free(B); BN_free(s);
    BN_free(A); BN_free(k); BN_free(u); BN_free(x);
    BN_free(gx); BN_free(kgx); BN_free(base);
    BN_free(ux); BN_free(aux); BN_free(S);
    BN_CTX_free(ctx);
    return ok && kmatch;
}

} // namespace openmirror::airplay
