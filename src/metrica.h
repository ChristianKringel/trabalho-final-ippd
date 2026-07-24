#ifndef METRICA_H
#define METRICA_H

#include <stdint.h>

// Metricas por bloco: cada processo calcula as suas e o rank 0 reune tudo num
// .csv. Tipos de tamanho fixo para a struct viajar como bytes no MPI_Gather
// sem surpresa de alinhamento.

typedef struct {
    int32_t rank;             // processo que gerou a metrica
    int32_t offset;           // inicio do bloco no audio global
    int32_t n;                // amostras do bloco
    double  rms;              // energia RMS (volume medio)
    double  pico;             // maior amplitude absoluta
    double  freq_dominante;   // frequencia dominante em Hz (via FFT)
} MetricaBloco;

// Calcula a metrica de um bloco. rank e offset sao informativos (vao para o
// CSV); sample_rate converte o bin da FFT em Hz.
MetricaBloco calcular_metrica(const float *bloco, int n, int rank, int offset,
                              uint32_t sample_rate);

// Escreve o .csv com uma linha por bloco. So o rank 0 deve chamar.
// Retorna 0 em sucesso.
int escrever_metricas_csv(const char *caminho, const MetricaBloco *metricas,
                          int n_blocos);

#endif // METRICA_H
