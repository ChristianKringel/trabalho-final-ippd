#include "metrica.h"
#include "fft.h"

#include <math.h>
#include <stdio.h>

// Calculo das metricas do bloco e escrita do CSV. RMS indica o volume medio,
// o pico ajuda a detectar clipping e a frequencia dominante mostra que tom
// predomina no trecho.

MetricaBloco calcular_metrica(const float *bloco, int n, int rank, int offset,
                              uint32_t sample_rate) {
    MetricaBloco m;
    m.rank = rank;
    m.offset = offset;
    m.n = n;

    double soma_quadrados = 0.0;
    double pico = 0.0;
    // Uma passada, as duas reducoes juntas. O 'max' e exato; a soma pode variar
    // no ultimo bit conforme as threads, o que nao muda o CSV (seis decimais).
    // O 'if' evita abrir a regiao paralela para bloco pequeno.
    #pragma omp parallel for schedule(static) if(n >= 65536) \
            reduction(+:soma_quadrados) reduction(max:pico)
    for (int i = 0; i < n; i++) {
        double v = (double)bloco[i];
        soma_quadrados += v * v;
        double abs_v = fabs(v);
        if (abs_v > pico) {
            pico = abs_v;
        }
    }

    m.rms = (n > 0) ? sqrt(soma_quadrados / n) : 0.0;
    m.pico = pico;
    m.freq_dominante = frequencia_dominante(bloco, n, sample_rate);

    return m;
}

int escrever_metricas_csv(const char *caminho, const MetricaBloco *metricas,
                          int n_blocos) {
    FILE *f = fopen(caminho, "w");
    if (!f) {
        fprintf(stderr, "Erro: nao consegui criar o arquivo de metricas '%s'.\n", caminho);
        return 1;
    }

    fprintf(f, "rank,offset,num_amostras,rms,pico,freq_dominante_hz\n");
    for (int i = 0; i < n_blocos; i++) {
        const MetricaBloco *m = &metricas[i];
        fprintf(f, "%d,%d,%d,%.6f,%.6f,%.2f\n",
                m->rank, m->offset, m->n, m->rms, m->pico, m->freq_dominante);
    }

    fclose(f);
    return 0;
}
