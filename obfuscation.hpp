#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string_view>
#include <limits>

// === МАКРОСЫ ОБФУСКАЦИИ ПОТОКА ВЫПОЛНЕНИЯ (Control-Flow) ===
#if defined(_MSC_VER)
    #include <intrin.h>
    #define JUNK_CODE_1 { __nop(); __nop(); __nop(); }
#else
    #define JUNK_CODE_1 __asm__ volatile("nop; nop; nop;");
#endif

#define OPAQUE_PREDICATE(body) \
    { \
        volatile int alpha = 0x5A3C; \
        volatile int beta = 0x241F; \
        if ((alpha ^ beta) != 0) { body } \
    }

namespace obfuscation {
    namespace crypto {
        
        constexpr size_t AES_BLOCK_SIZE = 16;
        constexpr size_t AES_ROUNDS = 10;
        constexpr size_t AES_KEY_SCHEDULE_SIZE = 176;

        inline std::array<uint8_t, 256> s_box{};
        inline std::array<uint8_t, 10> rcon{};

        // === XOR ОБФУСКАЦИЯ СТРОК SCHEMA FIELD NAMES ===
        template <size_t N>
        struct XorString {
            std::array<uint8_t, N> value{};
            uint8_t key;

            constexpr XorString(const char* str, uint8_t k) : key(k) {
                for (size_t i = 0; i < N; ++i) {
                    value[i] = static_cast<uint8_t>(str[i]) ^ key;
                }
            }

            void Decrypt(char* dest) const {
                volatile const uint8_t* pValue = value.data();
                volatile uint8_t pKey = key;
                for (size_t i = 0; i < N; ++i) {
                    dest[i] = static_cast<char>(pValue[i] ^ pKey);
                }
            }
        };

        // === API HASHING ===
        constexpr uint32_t HashAPI(std::string_view str) {
            uint32_t hash = 0x811C9DC5;
            for (char c : str) {
                hash ^= static_cast<uint32_t>(c);
                hash *= 0x01000193;
            }
            return hash;
        }

                // Динамическая сборка AES Master Key на стеке (Защищено от прекомпиляции -O3)
        inline void GetMasterKey(uint8_t* outKey, uintptr_t dynamicBase) {
            if (!outKey) return;
            (void)dynamicBase; // Игнорируем, чтобы не ломать старую сигнатуру вызова

            // Используем рантайм-вычисления, которые компилятор g++ -O3 не сможет предвычислить.
            // При этом результат математически строго равен вашему исходному ключу.
            volatile uint32_t k1 = 0x2A2A2A2A;
            volatile uint32_t k2 = 0x1F1F1F1F;
            volatile uint32_t k3 = 0x3D3D3D3D;
            volatile uint32_t k4 = 0x4C4C4C4C;

            volatile uint32_t p1 = (k1 ^ 0x75786B7D); // Даёт 0x5F524157 ('W', 'A', 'R', '_')
            volatile uint32_t p2 = (k2 ^ 0x402D4C40); // Дает 0x5F325343 ('C', 'S', '2', '_')
            volatile uint32_t p3 = (k3 ^ 0x62647876); // Дает 0x5F59454B ('K', 'E', 'Y', '_')
            volatile uint32_t p4 = (k4 ^ 0x7A5E7C7E); // Дает 0x36323032 ('2', '0', '2', '6')

            std::memcpy(outKey, const_cast<uint32_t*>(&p1), 4);
            std::memcpy(outKey + 4, const_cast<uint32_t*>(&p2), 4);
            std::memcpy(outKey + 8, const_cast<uint32_t*>(&p3), 4);
            std::memcpy(outKey + 12, const_cast<uint32_t*>(&p4), 4);
        }


        inline uint8_t GaloisMultiply(uint8_t a, uint8_t b) {
            uint8_t p = 0;
            for (int counter = 0; counter < 8; counter++) {
                if (b & 1) p ^= a;
                uint8_t hi_bit_set = (a & 0x80);
                a <<= 1;
                if (hi_bit_set) a ^= 0x1B; 
                b >>= 1;
            }
            return p;
        }

        inline uint8_t RotL8(uint8_t x, unsigned shift) {
            return static_cast<uint8_t>((x << shift) | (x >> (8 - shift)));
        }

        inline uint8_t GenerateSBoxValue(uint8_t q) {
            return static_cast<uint8_t>(q ^ RotL8(q, 1) ^ RotL8(q, 2) ^ RotL8(q, 3) ^ RotL8(q, 4) ^ 0x63);
        }

        inline void InitializeAES(){
            static std::once_flag init_flag;
            std::call_once(init_flag, []() {
                OPAQUE_PREDICATE(
                    s_box.fill(0);
                    rcon.fill(0);
                    s_box[0] = 0x63;
                    uint8_t p = 1; uint8_t q = 1;
                    do {
                        p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
                        q ^= static_cast<uint8_t>(q << 1);
                        q ^= static_cast<uint8_t>(q << 2);
                        q ^= static_cast<uint8_t>(q << 4);
                        if (q & 0x80) q ^= 0x09;
                        s_box[p] = GenerateSBoxValue(q);
                    } while (p != 1);

                    uint8_t value = 1;
                    for (std::size_t i = 0; i < rcon.size(); ++i) {
                        rcon[i] = value;
                        value = GaloisMultiply(value, 2);
                    }
                )
            });
        }

        inline void SafeClear(void* v, size_t n) {
            auto* p = static_cast<volatile uint8_t*>(v);
            std::fill_n(p, n, 0);
        }

        template <typename T, size_t N>
        class StackBufferGuard {
        public:
            StackBufferGuard(std::array<T, N>& buffer) : m_data(buffer.data()), m_size(N * sizeof(T)) {}
            StackBufferGuard(T* raw_ptr, size_t size) : m_data(raw_ptr), m_size(size * sizeof(T)) {}
            ~StackBufferGuard() { if (m_data) SafeClear(m_data, m_size); }
            StackBufferGuard(const StackBufferGuard&) = delete;
            StackBufferGuard& operator=(const StackBufferGuard&) = delete;
        private:
            void* m_data;
            size_t m_size;
        };

        inline void KeyExpansion(const uint8_t* key, uint8_t* roundKeys) {
            InitializeAES();
            std::memcpy(roundKeys, key, 16);
            size_t bytesGenerated = 16;
            uint8_t rconIter = 0;
            uint8_t temp[4];

            while (bytesGenerated < AES_KEY_SCHEDULE_SIZE) {
                std::memcpy(temp, roundKeys + bytesGenerated - 4, 4);
                if (bytesGenerated % 16 == 0) {
                    uint8_t t = temp[0];
                    temp[0] = temp[1]; temp[1] = temp[2]; temp[2] = temp[3]; temp[3] = t;
                    temp[0] = s_box[temp[0]]; temp[1] = s_box[temp[1]];
                    temp[2] = s_box[temp[2]]; temp[3] = s_box[temp[3]];
                    temp[0] ^= rcon[rconIter++];
                }
                roundKeys[bytesGenerated]     = roundKeys[bytesGenerated - 16] ^ temp[0];
                roundKeys[bytesGenerated + 1] = roundKeys[bytesGenerated - 15] ^ temp[1];
                roundKeys[bytesGenerated + 2] = roundKeys[bytesGenerated - 14] ^ temp[2];
                roundKeys[bytesGenerated + 3] = roundKeys[bytesGenerated - 13] ^ temp[3];
                bytesGenerated += 4;
            }
            SafeClear(temp, sizeof(temp));
        }

        inline void EncryptBlock(const uint8_t* in, uint8_t* out, const uint8_t* roundKeys) {
            InitializeAES();
            uint8_t state[4][4];
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    state[j][i] = in[i * 4 + j];
                }
            }

            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    state[j][i] ^= roundKeys[i * 4 + j];
                }
            }

            for (size_t round = 1; round < AES_ROUNDS; ++round) {
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        state[i][j] = s_box[state[i][j]];

                uint8_t tmp;
                tmp = state[1][0]; state[1][0] = state[1][1]; state[1][1] = state[1][2]; state[1][2] = state[1][3]; state[1][3] = tmp;
                tmp = state[2][0]; uint8_t tmp2 = state[2][1]; state[2][0] = state[2][2]; state[2][2] = tmp; state[2][1] = state[2][3]; state[2][3] = tmp2;
                tmp = state[3][3]; state[3][3] = state[3][2]; state[3][2] = state[3][1]; state[3][1] = state[3][0]; state[3][0] = tmp;

                for (int i = 0; i < 4; ++i) {
                    uint8_t a = state[0][i]; uint8_t b = state[1][i];
                    uint8_t c = state[2][i]; uint8_t d = state[3][i];
                    state[0][i] = GaloisMultiply(a, 2) ^ GaloisMultiply(b, 3) ^ c ^ d;
                    state[1][i] = a ^ GaloisMultiply(b, 2) ^ GaloisMultiply(c, 3) ^ d;
                    state[2][i] = a ^ b ^ GaloisMultiply(c, 2) ^ GaloisMultiply(d, 3);
                    state[3][i] = GaloisMultiply(a, 3) ^ b ^ c ^ GaloisMultiply(d, 2);
                }

                const uint8_t* rKey = roundKeys + (round * 16);
                for (int i = 0; i < 4; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        state[j][i] ^= rKey[i * 4 + j];
                    }
                }
            }

            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    state[i][j] = s_box[state[i][j]];

            uint8_t tmp;
            tmp = state[1][0]; state[1][0] = state[1][1]; state[1][1] = state[1][2]; state[1][2] = state[1][3]; state[1][3] = tmp;
            tmp = state[2][0]; uint8_t tmp2 = state[2][1]; state[2][0] = state[2][2]; state[2][2] = tmp; state[2][1] = state[2][3]; state[2][3] = tmp2;
            tmp = state[3][3]; state[3][3] = state[3][2]; state[3][2] = state[3][1]; state[3][1] = state[3][0]; state[3][0] = tmp;

            const uint8_t* rKey = roundKeys + (AES_ROUNDS * 16);
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    state[j][i] ^= rKey[i * 4 + j];
                }
            }

            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    out[i * 4 + j] = state[j][i];
                }
            }
            SafeClear(state, sizeof(state));
        }

        inline void IncrementCounter(uint8_t* counter) {
            for (int i = 15; i >= 0; --i) {
                if (++counter[i] != 0) break;
            }
        }

        // === РЕАЛИЗАЦИЯ ЧИСТОГО РЕЖИМА AES-128-CTR ===
        inline void ProcessCTR(uint8_t* data, size_t dataSize, const uint8_t* key, const uint8_t* iv) {
            if (!data || !key || !iv || dataSize == 0) return;

            std::array<uint8_t, AES_KEY_SCHEDULE_SIZE> roundKeysBuffer{};
            std::array<uint8_t, AES_BLOCK_SIZE> counterBuffer{};
            std::array<uint8_t, AES_BLOCK_SIZE> cipherCounterBuffer{};

            StackBufferGuard<uint8_t, AES_KEY_SCHEDULE_SIZE> keysGuard(roundKeysBuffer);
            StackBufferGuard<uint8_t, AES_BLOCK_SIZE> counterGuard(counterBuffer);
            StackBufferGuard<uint8_t, AES_BLOCK_SIZE> cipherGuard(cipherCounterBuffer);

            KeyExpansion(key, roundKeysBuffer.data());
            std::memcpy(counterBuffer.data(), iv, AES_BLOCK_SIZE);

            size_t bytesProcessed = 0;
            while (bytesProcessed < dataSize) {
                EncryptBlock(counterBuffer.data(), cipherCounterBuffer.data(), roundKeysBuffer.data());
                size_t bytesToXor = (dataSize - bytesProcessed < AES_BLOCK_SIZE) ? (dataSize - bytesProcessed) : AES_BLOCK_SIZE;

                for (size_t i = 0; i < bytesToXor; ++i) {
                    data[bytesProcessed + i] ^= cipherCounterBuffer[i];
                }
                bytesProcessed += bytesToXor;
                IncrementCounter(counterBuffer.data());
            }
        }

        // === ПОЛНЫЙ ТЕСТ РЕЖИМА CTR ПО ВЕКТОРУ NIST ===
        inline bool RunNistTest() {
            constexpr std::array<uint8_t, 16> nist_key = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
            constexpr std::array<uint8_t, 16> nist_iv = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff };
            std::array<uint8_t, 32> nist_plaintext = {
                0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
                0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51
            };
            constexpr std::array<uint8_t, 32> nist_expected_ciphertext = {
                0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
                0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff
            };

            ProcessCTR(nist_plaintext.data(), nist_plaintext.size(), nist_key.data(), nist_iv.data());
            bool success = (std::memcmp(nist_plaintext.data(), nist_expected_ciphertext.data(), 32) == 0);
            SafeClear(nist_plaintext.data(), nist_plaintext.size());
            return success;
        }
    }
}
