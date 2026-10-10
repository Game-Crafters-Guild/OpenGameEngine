#pragma once

#include <string>

namespace GameEngine::Editor
{
    // Simple token encryption/decryption for storing sensitive tokens
    // Uses XOR cipher with a key derived from machine-specific data
    // This provides basic obfuscation - not cryptographically secure, but better than plain text
    class TokenEncryption
    {
    public:
        // Encrypt a token for storage
        static std::string Encrypt(const std::string& token);
        
        // Decrypt a stored token
        static std::string Decrypt(const std::string& encryptedToken);
        
        // Check if a string appears to be encrypted (heuristic check)
        static bool IsEncrypted(const std::string& value);
        
    private:
        // Generate a machine-specific encryption key
        static std::string GenerateKey();
    };
}
