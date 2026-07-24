#include "fft.h"

#include <math.h>
#include <stdlib.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// FFT Cooley-Tukey radix-2: reordena por bit invertido e combina blocos que
// dobram de tamanho a cada etapa (2, 4, 8, ..., n).
//
// O laco das etapas e serial: cada uma consome o resultado da anterior. O
// paralelismo esta dentro da etapa, nas n/2 borboletas, que tocam pares de
// indices disjuntos. Os caminhos serial e paralelo dao o mesmo resultado, entao
// a saida nao depende do numero de threads.

// Abaixo deste tamanho a FFT roda serial: montar a tabela e sincronizar as
// barreiras custa mais que o calculo.
#define FFT_MIN_PARALELO 4096

static int threads_disponiveis(void) {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

// Fase 1 serial: j vem do valor anterior. Mais barato em uma thread, mas e uma
// recorrencia, logo nao paraleliza.
static void permutar_bits(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
}

// Fase 1 paralela: cada posicao calcula o proprio indice invertido, sem herdar
// do anterior. Custa ~log2(n) ops inteiras por amostra contra ~2, entao so
// compensa com 4 threads ou mais (medido em n = 2^20).
static void permutar_bits_paralelo(double *re, double *im, int n, int bits) {
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++) {
        unsigned v = (unsigned)i;
        unsigned r = 0;
        for (int b = 0; b < bits; b++) {
            r = (r << 1) | (v & 1u);
            v >>= 1;
        }
        int j = (int)r;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
}

// Fase 2 serial, com twiddle recorrente. Usada com poucas threads, bloco
// pequeno, ou se faltar memoria para a tabela.
static void combinar_serial(double *re, double *im, int n) {
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;   // negativo = transformada direta
        double w_re = cos(ang);
        double w_im = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cur_re = 1.0;   // twiddle atual
            double cur_im = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k;
                int b = i + k + len / 2;

                double u_re = re[a];
                double u_im = im[a];
                double v_re = re[b] * cur_re - im[b] * cur_im;
                double v_im = re[b] * cur_im + im[b] * cur_re;

                re[a] = u_re + v_re;
                im[a] = u_im + v_im;
                re[b] = u_re - v_re;
                im[b] = u_im - v_im;

                double novo_re = cur_re * w_re - cur_im * w_im;
                double novo_im = cur_re * w_im + cur_im * w_re;
                cur_re = novo_re;
                cur_im = novo_im;
            }
        }
    }
}

void fft(double *re, double *im, int n) {
    if (n < 2) {
        return;
    }

    int n_threads = threads_disponiveis();
    int vale_paralelizar = (n_threads >= 2) && (n >= FFT_MIN_PARALELO);

    if (vale_paralelizar && n_threads >= 4) {
        int bits = 0;
        while ((1 << bits) < n) {
            bits++;
        }
        permutar_bits_paralelo(re, im, n, bits);
    } else {
        permutar_bits(re, im, n);
    }

    if (!vale_paralelizar) {
        combinar_serial(re, im, n);
        return;
    }

    // Tabela de twiddles por etapa: exp(-2*pi*i*k/len) fica em tw[len/2 + k].
    // Esse layout deixa a leitura contigua no laco das borboletas; guardar uma
    // tabela unica indexada por k*(n/len) fica estriado e mede 39% mais lento.
    double *tw_re = malloc(n * sizeof(double));
    double *tw_im = malloc(n * sizeof(double));
    if (!tw_re || !tw_im) {
        free(tw_re);
        free(tw_im);
        combinar_serial(re, im, n);
        return;
    }

    int metade = n / 2;   // borboletas por etapa (constante)

    // Uma unica regiao paralela para a tabela e para todas as etapas. Com uma
    // regiao por etapa eram 2*log2(n) criacoes de equipe de threads, e com mais
    // threads que nucleos isso deixou a FFT 20x mais lenta que a serial. Cada
    // thread percorre o laco de 'len' de forma redundante; o trabalho e
    // repartido pelos 'omp for'.
    #pragma omp parallel
    {
        // 'nowait' e seguro: cada etapa escreve numa faixa propria de indices.
        for (int len = 2; len <= n; len <<= 1) {
            int half = len >> 1;
            #pragma omp for schedule(static) nowait
            for (int k = 0; k < half; k++) {
                double ang = -2.0 * M_PI * (double)k / (double)len;
                tw_re[half + k] = cos(ang);
                tw_im[half + k] = sin(ang);
            }
        }

        #pragma omp barrier   // tabela pronta antes da primeira borboleta

        // 'log_half' segue log2(len/2) para trocar as divisoes por
        // deslocamentos: uma divisao por variavel custaria mais que a borboleta.
        int log_half = 0;
        for (int len = 2; len <= n; len <<= 1, log_half++) {
            int half = len >> 1;

            // Sem 'nowait' de proposito: a barreira do fim do 'omp for' e a
            // sincronizacao que a dependencia entre etapas exige.
            #pragma omp for schedule(static)
            for (int j = 0; j < metade; j++) {
                int k = j & (half - 1);   // posicao dentro do bloco
                int a = ((j >> log_half) << (log_half + 1)) + k;
                int b = a + half;

                double w_re = tw_re[half + k];
                double w_im = tw_im[half + k];

                double u_re = re[a];
                double u_im = im[a];
                double v_re = re[b] * w_re - im[b] * w_im;
                double v_im = re[b] * w_im + im[b] * w_re;

                re[a] = u_re + v_re;
                im[a] = u_im + v_im;
                re[b] = u_re - v_re;
                im[b] = u_im - v_im;
            }
        }
    }

    free(tw_re);
    free(tw_im);
}

int maior_potencia_2(int n) {
    if (n < 1) {
        return 0;
    }
    int p = 1;
    while (p * 2 <= n) {
        p *= 2;
    }
    return p;
}

double frequencia_dominante(const float *bloco, int n, uint32_t sample_rate) {
    int tam = maior_potencia_2(n);
    if (tam < 2) {
        return 0.0;
    }

    double *re = malloc(tam * sizeof(double));
    double *im = malloc(tam * sizeof(double));
    if (!re || !im) {
        free(re);
        free(im);
        return 0.0;
    }

    // Copia as primeiras 'tam' amostras com janela de Hann
    #pragma omp parallel for schedule(static) if(tam >= FFT_MIN_PARALELO)
    for (int i = 0; i < tam; i++) {
        double janela = 0.5 * (1.0 - cos(2.0 * M_PI * i / (tam - 1)));
        re[i] = (double)bloco[i] * janela;
        im[i] = 0.0;
    }

    fft(re, im, tam);

    // Bin de maior magnitude na primeira metade (a outra e espelho, sinal real).
    // O bin k corresponde a k*fs/tam. OpenMP nao tem reduction de argmax: cada
    // thread guarda o melhor da sua faixa e a combinacao entra em 'critical',
    // uma vez por thread. Empate fica com o bin menor, como no serial.
    int melhor_bin = 0;
    double melhor_mag = -1.0;

    #pragma omp parallel if(tam >= FFT_MIN_PARALELO)
    {
        int bin_local = 0;
        double mag_local = -1.0;

        #pragma omp for schedule(static)
        for (int k = 1; k < tam / 2; k++) {
            double mag = re[k] * re[k] + im[k] * im[k];
            if (mag > mag_local) {
                mag_local = mag;
                bin_local = k;
            }
        }

        #pragma omp critical
        {
            if (mag_local > melhor_mag ||
                (mag_local == melhor_mag && bin_local < melhor_bin)) {
                melhor_mag = mag_local;
                melhor_bin = bin_local;
            }
        }
    }

    free(re);
    free(im);

    return (double)melhor_bin * (double)sample_rate / (double)tam;
}
