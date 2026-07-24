#include "wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Parser do .wav feito a mao, sem lib externa. So PCM 16 bits.
// Todos os campos numericos do WAV sao little-endian.

static uint16_t le_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le_u32(const unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void escreve_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void escreve_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

int ler_wav(const char *caminho, WavInfo *info, float **amostras) {
    *amostras = NULL;

    FILE *f = fopen(caminho, "rb");
    if (!f) {
        fprintf(stderr, "Erro: nao consegui abrir o arquivo de entrada '%s'.\n", caminho);
        return 1;
    }

    // Cabecalho RIFF: "RIFF" <tamanho> "WAVE"
    unsigned char riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Erro: '%s' nao parece ser um arquivo WAV valido (RIFF/WAVE).\n", caminho);
        fclose(f);
        return 1;
    }

    // Daqui em diante o arquivo e uma sequencia de chunks (id + tamanho, 4
    // bytes cada). Interessam o "fmt " e o "data"; os outros sao pulados.
    int achou_fmt = 0;
    uint16_t formato_audio = 0;
    uint16_t num_canais = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_amostra = 0;

    unsigned char cab[8];
    while (fread(cab, 1, 8, f) == 8) {
        uint32_t tam_chunk = le_u32(cab + 4);

        if (memcmp(cab, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (tam_chunk < 16 || fread(fmt, 1, 16, f) != 16) {
                fprintf(stderr, "Erro: chunk 'fmt ' de '%s' esta truncado.\n", caminho);
                fclose(f);
                return 1;
            }
            formato_audio = le_u16(fmt + 0);
            num_canais    = le_u16(fmt + 2);
            sample_rate   = le_u32(fmt + 4);
            bits_amostra  = le_u16(fmt + 14);
            achou_fmt = 1;

            if (tam_chunk > 16) {
                fseek(f, (long)(tam_chunk - 16), SEEK_CUR);
            }
        } else if (memcmp(cab, "data", 4) == 0) {
            if (!achou_fmt) {
                fprintf(stderr, "Erro: chunk 'data' apareceu antes de 'fmt ' em '%s'.\n", caminho);
                fclose(f);
                return 1;
            }
            if (formato_audio != 1) {
                fprintf(stderr, "Erro: '%s' nao e PCM (formato %u). So suportamos PCM.\n",
                        caminho, formato_audio);
                fclose(f);
                return 1;
            }
            if (bits_amostra != 16) {
                fprintf(stderr, "Erro: '%s' tem %u bits por amostra. So suportamos 16 bits.\n",
                        caminho, bits_amostra);
                fclose(f);
                return 1;
            }
            if (num_canais < 1) {
                fprintf(stderr, "Erro: numero de canais invalido (%u) em '%s'.\n",
                        num_canais, caminho);
                fclose(f);
                return 1;
            }

            uint32_t total_valores = tam_chunk / 2;   // 2 bytes por amostra

            int16_t *pcm = malloc(total_valores * sizeof(int16_t));
            if (!pcm) {
                fprintf(stderr, "Erro: sem memoria para ler as amostras de '%s'.\n", caminho);
                fclose(f);
                return 1;
            }
            if (fread(pcm, sizeof(int16_t), total_valores, f) != total_valores) {
                fprintf(stderr, "Erro: nao consegui ler todas as amostras de '%s'.\n", caminho);
                free(pcm);
                fclose(f);
                return 1;
            }

            float *saida = malloc(total_valores * sizeof(float));
            if (!saida) {
                fprintf(stderr, "Erro: sem memoria para converter as amostras de '%s'.\n", caminho);
                free(pcm);
                fclose(f);
                return 1;
            }
            // int16 -> float normalizado em [-1, 1]
            for (uint32_t i = 0; i < total_valores; i++) {
                saida[i] = (float)pcm[i] / 32768.0f;
            }
            free(pcm);
            fclose(f);

            info->sample_rate  = sample_rate;
            info->num_canais   = num_canais;
            info->bits_amostra = bits_amostra;
            info->num_amostras = total_valores / num_canais;
            *amostras = saida;
            return 0;
        } else {
            // Chunk desconhecido: pula o conteudo (+1 byte de padding se impar)
            fseek(f, (long)tam_chunk, SEEK_CUR);
            if (tam_chunk % 2 == 1) {
                fseek(f, 1, SEEK_CUR);
            }
        }
    }

    fprintf(stderr, "Erro: nao encontrei o chunk 'data' em '%s'.\n", caminho);
    fclose(f);
    return 1;
}

int escrever_wav(const char *caminho, const WavInfo *info, const float *amostras) {
    FILE *f = fopen(caminho, "wb");
    if (!f) {
        fprintf(stderr, "Erro: nao consegui criar o arquivo de saida '%s'.\n", caminho);
        return 1;
    }

    uint32_t total_valores = info->num_amostras * info->num_canais;
    uint16_t bytes_por_amostra = info->bits_amostra / 8;
    uint32_t tam_data = total_valores * bytes_por_amostra;
    uint32_t byte_rate = info->sample_rate * info->num_canais * bytes_por_amostra;
    uint16_t block_align = info->num_canais * bytes_por_amostra;

    // Cabecalho canonico de 44 bytes
    unsigned char cab[44];
    memcpy(cab + 0, "RIFF", 4);
    escreve_u32(cab + 4, 36 + tam_data);   // tamanho do arquivo menos 8
    memcpy(cab + 8, "WAVE", 4);
    memcpy(cab + 12, "fmt ", 4);
    escreve_u32(cab + 16, 16);             // tamanho do subchunk fmt
    escreve_u16(cab + 20, 1);              // formato PCM
    escreve_u16(cab + 22, info->num_canais);
    escreve_u32(cab + 24, info->sample_rate);
    escreve_u32(cab + 28, byte_rate);
    escreve_u16(cab + 32, block_align);
    escreve_u16(cab + 34, info->bits_amostra);
    memcpy(cab + 36, "data", 4);
    escreve_u32(cab + 40, tam_data);

    if (fwrite(cab, 1, 44, f) != 44) {
        fprintf(stderr, "Erro: falha ao escrever o cabecalho de '%s'.\n", caminho);
        fclose(f);
        return 1;
    }

    int16_t *pcm = malloc(total_valores * sizeof(int16_t));
    if (!pcm) {
        fprintf(stderr, "Erro: sem memoria para escrever as amostras de '%s'.\n", caminho);
        fclose(f);
        return 1;
    }
    // float -> int16 com clipping, na mesma escala da leitura (32768) para o
    // ciclo escrever->ler nao introduzir vies
    for (uint32_t i = 0; i < total_valores; i++) {
        float v = amostras[i];
        if (v > 1.0f)  v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int amostra = (int)(v * 32768.0f + (v >= 0.0f ? 0.5f : -0.5f));
        if (amostra > 32767)  amostra = 32767;
        if (amostra < -32768) amostra = -32768;
        pcm[i] = (int16_t)amostra;
    }

    if (fwrite(pcm, sizeof(int16_t), total_valores, f) != total_valores) {
        fprintf(stderr, "Erro: falha ao escrever as amostras de '%s'.\n", caminho);
        free(pcm);
        fclose(f);
        return 1;
    }

    free(pcm);
    fclose(f);
    return 0;
}
