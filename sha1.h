#pragma once
#include <string.h>
#include <string>
#include <sstream> // Добавлено для формирования hex-строки
#include <iomanip> // Добавлено для std::hex и std::setw

#define one_block_size_bytes 64 
#define one_block_size_uints 16 
#define block_expend_size_uints 80 

#define SHA1HASHLENGTHBYTES 20
#define SHA1HASHLENGTHUINTS 5

using namespace std;

namespace MySha {
    typedef unsigned int uint;
    typedef uint* Block;
    typedef uint ExpendBlock[block_expend_size_uints];

    static const uint H[5] = {
        0x67452301,
        0xEFCDAB89,
        0x98BADCFE,
        0x10325476,
        0xC3D2E1F0
    };

    inline uint cycle_shift_left(uint val, int bit_count) {
        return (val << bit_count | val >> (32 - bit_count));
    }

    inline uint bring_to_human_view(uint val) {
        return  ((val & 0x000000FF) << 24) |
                ((val & 0x0000FF00) << 8) |
                ((val & 0x00FF0000) >> 8) |
                ((val & 0xFF000000) >> 24);
    }

    inline std::string sha1(const std::string& str_message) {
        const char* message = str_message.c_str();
        uint msize_bytes = (uint)str_message.size();

        // Инициализация накопителей
        uint A = H[0];
        uint B = H[1];
        uint C = H[2];
        uint D = H[3];
        uint E = H[4];

        // Расчет количества блоков
        uint totalBlockCount = msize_bytes / one_block_size_bytes;
        uint needAdditionalBytes = one_block_size_bytes - (msize_bytes - totalBlockCount * one_block_size_bytes);

        if (needAdditionalBytes < 8) {
            totalBlockCount += 2;
            needAdditionalBytes += one_block_size_bytes;
        } else {
            totalBlockCount += 1;
        }

        uint extendedMessageSize = msize_bytes + needAdditionalBytes;

        // Выделение памяти и копирование
        unsigned char* newMessage = new unsigned char[extendedMessageSize];
        memcpy(newMessage, message, msize_bytes);

        // Добавление бита '1'
        newMessage[msize_bytes] = 0x80;
        memset(newMessage + msize_bytes + 1, 0, needAdditionalBytes - 1);

        // Запись длины сообщения в конец (big-endian)
        uint* ptr_to_size = (uint*)(newMessage + extendedMessageSize - 4);
        *ptr_to_size = bring_to_human_view(msize_bytes * 8);

        ExpendBlock exp_block;

        for (int i = 0; i < totalBlockCount; i++) {
            unsigned char* cur_p = newMessage + one_block_size_bytes * i;
            Block block = (Block)cur_p;

            // Подготовка 80 слов
            for (int j = 0; j < one_block_size_uints; j++) {
                exp_block[j] = bring_to_human_view(block[j]);
            }
            for (int j = one_block_size_uints; j < block_expend_size_uints; j++) {
                exp_block[j] = cycle_shift_left(
                    exp_block[j - 3] ^ exp_block[j - 8] ^ exp_block[j - 14] ^ exp_block[j - 16], 
                    1
                );
            }

            // Инициализация переменных сжатия ТЕКУЩИМ состоянием (а не константами H)
            uint a = A;
            uint b = B;
            uint c = C;
            uint d = D;
            uint e = E;

            // Основной цикл сжатия
            for (int j = 0; j < block_expend_size_uints; j++) {
                uint f, k;
                if (j < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (j < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (j < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }

                uint temp = cycle_shift_left(a, 5) + f + e + k + exp_block[j];
                e = d;
                d = c;
                c = cycle_shift_left(b, 30);
                b = a;
                a = temp;
            }

            // Обновление глобального состояния
            A += a;
            B += b;
            C += c;
            D += d;
            E += e;
        }

        delete[] newMessage;

        // Формирование итоговой HEX-строки
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        ss << std::setw(8) << A << std::setw(8) << B << std::setw(8) << C 
           << std::setw(8) << D << std::setw(8) << E;

        return ss.str();
    }
}