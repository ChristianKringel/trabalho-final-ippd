#include "filtros.h"

#include <math.h>
#include <omp.h>
#include <stdio.h>

// Implementacao dos filtros. Cada um percorre o bloco local dividindo as
// amostras entre as threads da regiao paralela.
//
// A faixa de cada thread e calculada a mao (faixa_da_thread) em vez de deixar o
// 'omp for' fazer isso, porque o log precisa saber onde a faixa comeca e termina.
// Fazendo isso com contadores dentro do laco, como antes, atrapalhava a
// vetorizacao; fora dele o log sai de graca.

static int g_verbose = 0;

void filtros_set_verbose(int ativo) {
    g_verbose = ativo;
}

// Divide [0, n) entre as threads como 'schedule(static)': as primeiras 'resto'
// levam uma amostra a mais, entao a diferenca entre faixas e no maximo 1.
static void faixa_da_thread(int n, int *inicio, int *fim) {
    int n_threads = omp_get_num_threads();
    int tid       = omp_get_thread_num();

    int base  = n / n_threads;
    int resto = n % n_threads;

    int ini = tid * base + (tid < resto ? tid : resto);
    int qtd = base + (tid < resto ? 1 : 0);

    *inicio = ini;
    *fim    = ini + qtd;
}

// Uma linha por thread por filtro, so com o verbose ligado: e I/O de terminal
// dentro da regiao paralela, e as threads disputam o lock do stdout.
static void log_faixa(int rank, const char *nome, int inicio, int fim) {
    if (g_verbose && fim > inicio) {
        printf("[Rank %d/Thread %d] %s: amostras %d-%d\n",
               rank, omp_get_thread_num(), nome, inicio, fim - 1);
    }
}

void aplicar_filtro_ganho(float *bloco, int n, float ganho, int rank) {
    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        for (int i = inicio; i < fim; i++) {
            bloco[i] = bloco[i] * ganho;
        }

        log_faixa(rank, "ganho", inicio, fim);
    }
}

void aplicar_threshold_ruido(float *bloco, int n, float limiar, int rank) {
    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        for (int i = inicio; i < fim; i++) {
            if (fabsf(bloco[i]) < limiar) {
                bloco[i] = 0.0f;
            }
        }

        log_faixa(rank, "threshold", inicio, fim);
    }
}

void aplicar_convolucao_media(const float *estendido, float *saida, int n,
                              int raio, int rank) {
    int comprimento = 2 * raio + 1;

    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        // saida[i] usa estendido[i .. i + 2*raio]; o centro da janela e
        // estendido[i + raio], que corresponde a amostra i do bloco
        for (int i = inicio; i < fim; i++) {
            double soma = 0.0;
            for (int k = 0; k < comprimento; k++) {
                soma += estendido[i + k];
            }
            saida[i] = (float)(soma / comprimento);
        }

        log_faixa(rank, "convolucao", inicio, fim);
    }
}

void aplicar_passa_alta_media(const float *estendido, float *saida, int n,
                              int raio, int rank) {
    int comprimento = 2 * raio + 1;

    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        // Media movel (graves) subtraida do valor original no centro da janela
        for (int i = inicio; i < fim; i++) {
            double soma = 0.0;
            for (int k = 0; k < comprimento; k++) {
                soma += estendido[i + k];
            }
            double media = soma / comprimento;
            saida[i] = (float)((double)estendido[i + raio] - media);
        }

        log_faixa(rank, "passa-alta", inicio, fim);
    }
}

void aplicar_eco(const float *estendido, float *saida, int n, int atraso,
                 float atenuacao, int rank) {
    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        // Com halo de 'atraso' amostras a esquerda: x[i] esta em
        // estendido[i + atraso] e x[i - atraso] em estendido[i]. No inicio
        // global o halo e zero, entao ali nao ha eco. O laco so le do
        // 'estendido', logo as threads nao interferem entre si.
        for (int i = inicio; i < fim; i++) {
            saida[i] = estendido[i + atraso] + atenuacao * estendido[i];
        }

        log_faixa(rank, "eco", inicio, fim);
    }
}

float pico_local_bloco(const float *bloco, int n) {
    float pico = 0.0f;
    // reduction(max:pico) da a cada thread uma copia privada e combina no fim,
    // sem condicao de corrida. Pico LOCAL; o global sai do Allreduce no main.
    #pragma omp parallel for schedule(static) reduction(max:pico)
    for (int i = 0; i < n; i++) {
        float a = fabsf(bloco[i]);
        if (a > pico) {
            pico = a;
        }
    }
    return pico;
}

void aplicar_normalizacao(float *bloco, int n, float fator, int rank) {
    #pragma omp parallel
    {
        int inicio, fim;
        faixa_da_thread(n, &inicio, &fim);

        for (int i = inicio; i < fim; i++) {
            bloco[i] = bloco[i] * fator;
        }

        log_faixa(rank, "normalizacao", inicio, fim);
    }
}
