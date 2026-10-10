#include "Jobs/WorkspaceId.h"
#include <array>
#include <sstream>
#include <filesystem>
#include <vector>
#include <iomanip>

namespace GameEngine {

// Very small SHA-1 implementation for stable ID (not for cryptographic security)
static void Sha1(const uint8_t* data, size_t len, uint8_t out[20]);

std::string WorkspaceId::Compute(const std::string& projectRootAbsUtf8)
{
    std::filesystem::path p(projectRootAbsUtf8);
    std::string norm = std::filesystem::weakly_canonical(p).string();
    uint8_t hash[20];
    Sha1(reinterpret_cast<const uint8_t*>(norm.data()), norm.size(), hash);
    std::ostringstream oss;
    oss << std::hex << std::nouppercase;
    for (int i = 0; i < 8; ++i) {
        oss << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}

// Minimal SHA-1 (RFC 3174) — trimmed and not timing-safe; good enough for IDs
static inline uint32_t rol(uint32_t x, int s){ return (x<<s)|(x>>(32-s)); }
static void Sha1(const uint8_t* data, size_t len, uint8_t out[20]){
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    size_t ml = len*8; std::vector<uint8_t> msg(data, data+len);
    msg.push_back(0x80);
    while ((msg.size()%64)!=56) msg.push_back(0);
    for(int i=7;i>=0;--i) msg.push_back((ml>>(i*8))&0xFF);
    for(size_t c=0;c<msg.size();c+=64){
        uint32_t w[80]; for(int i=0;i<16;++i) w[i]=(msg[c+4*i]<<24)|(msg[c+4*i+1]<<16)|(msg[c+4*i+2]<<8)|msg[c+4*i+3];
        for(int i=16;i<80;++i) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h0,b=h1,c2=h2,d=h3,e=h4;
        for(int i=0;i<80;++i){
            uint32_t f,k;
            if(i<20){f=(b&c2)|((~b)&d);k=0x5A827999;}
            else if(i<40){f=b^c2^d;k=0x6ED9EBA1;}
            else if(i<60){f=(b&c2)|(b&d)|(c2&d);k=0x8F1BBCDC;}
            else {f=b^c2^d;k=0xCA62C1D6;}
            uint32_t temp=rol(a,5)+f+e+k+w[i]; e=d; d=c2; c2=rol(b,30); b=a; a=temp;
        }
        h0+=a;h1+=b;h2+=c2;h3+=d;h4+=e;
    }
    uint32_t hv[5]={h0,h1,h2,h3,h4};
    for(int i=0;i<5;++i){ out[4*i]=(hv[i]>>24)&0xFF; out[4*i+1]=(hv[i]>>16)&0xFF; out[4*i+2]=(hv[i]>>8)&0xFF; out[4*i+3]=hv[i]&0xFF; }
}

} // namespace GameEngine

