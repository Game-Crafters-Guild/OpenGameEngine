#include "TokenEncryption.h"
#include <cstring>
#include <string>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <Lmcons.h> // UNLEN
#else
#include <unistd.h>
#include <sys/utsname.h>
#include <pwd.h>
#endif

namespace GameEngine::Editor
{
    std::string TokenEncryption::GenerateKey()
    {
        // Generate a key based on machine-specific data
        // This ensures the encryption is tied to this machine
        std::string keySource;
        
#ifdef _WIN32
        // Windows: Use computer name and username
        char computerName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD size = sizeof(computerName);
        if (GetComputerNameA(computerName, &size))
        {
            keySource += computerName;
        }
        
        char userName[UNLEN + 1] = {0};
        size = static_cast<DWORD>(sizeof(userName));
        if (GetUserNameA(userName, &size))
        {
            keySource += userName;
        }
#else
        // Unix/macOS: Use hostname and username
        char hostname[256] = {0};
        if (gethostname(hostname, sizeof(hostname)) == 0)
        {
            keySource += hostname;
        }
        
        struct passwd* pw = getpwuid(geteuid());
        if (pw && pw->pw_name)
        {
            keySource += pw->pw_name;
        }
#endif
        
        // If we couldn't get machine-specific data, use a fallback
        if (keySource.empty())
        {
            keySource = "GameEngine-Diversion-Token-Key";
        }
        
        // Create a deterministic key from the source
        // Use a simple hash-like approach to create a fixed-size key
        std::string key;
        key.reserve(32);
        
        // Create a 32-byte key by repeating and hashing the source
        for (size_t i = 0; i < 32; ++i)
        {
            char c = keySource[i % keySource.length()];
            // Mix with position to avoid simple repetition
            c = static_cast<char>((c ^ static_cast<char>(i)) & 0xFF);
            key += c;
        }
        
        return key;
    }
    
    std::string TokenEncryption::Encrypt(const std::string& token)
    {
        if (token.empty())
        {
            return "";
        }
        
        // Don't encrypt if it looks already encrypted (starts with "ENC:")
        if (token.length() > 4 && token.substr(0, 4) == "ENC:")
        {
            return token;
        }
        
        std::string key = GenerateKey();
        std::string encrypted;
        encrypted.reserve(token.length() + 4);
        encrypted = "ENC:"; // Prefix to mark as encrypted
        
        // XOR encryption
        for (size_t i = 0; i < token.length(); ++i)
        {
            char encryptedChar = token[i] ^ key[i % key.length()];
            // Convert to hex representation for safe storage
            char hex[3];
            snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned char>(encryptedChar));
            encrypted += hex;
        }
        
        return encrypted;
    }
    
    std::string TokenEncryption::Decrypt(const std::string& encryptedToken)
    {
        if (encryptedToken.empty())
        {
            return "";
        }
        
        // Check if it's encrypted (starts with "ENC:")
        if (encryptedToken.length() <= 4 || encryptedToken.substr(0, 4) != "ENC:")
        {
            // Not encrypted, return as-is (for backward compatibility)
            return encryptedToken;
        }
        
        // Remove "ENC:" prefix
        std::string hexData = encryptedToken.substr(4);
        
        // Must have even number of hex characters
        if (hexData.length() % 2 != 0)
        {
            return ""; // Invalid format
        }
        
        // Convert hex to bytes
        std::string encrypted;
        encrypted.reserve(hexData.length() / 2);
        for (size_t i = 0; i < hexData.length(); i += 2)
        {
            std::string hexByte = hexData.substr(i, 2);
            unsigned long byteValue = 0;
            try
            {
                byteValue = std::stoul(hexByte, nullptr, 16);
            }
            catch (...)
            {
                return ""; // Invalid hex format
            }
            if (byteValue > 255)
            {
                return ""; // Invalid byte value
            }
            encrypted += static_cast<char>(byteValue);
        }
        
        // Decrypt using XOR
        std::string key = GenerateKey();
        std::string decrypted;
        decrypted.reserve(encrypted.length());
        
        for (size_t i = 0; i < encrypted.length(); ++i)
        {
            char decryptedChar = encrypted[i] ^ key[i % key.length()];
            decrypted += decryptedChar;
        }
        
        return decrypted;
    }
    
    bool TokenEncryption::IsEncrypted(const std::string& value)
    {
        return value.length() > 4 && value.substr(0, 4) == "ENC:";
    }
}
