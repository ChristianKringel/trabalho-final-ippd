#include "mpi_utils.h"

#include <mpi.h>
#include <string.h>

// Particao dos dados entre os processos e troca de halo com os vizinhos.

void calcular_particao(int n_total, int n_procs, int *counts, int *displs) {
    int base = n_total / n_procs;
    int resto = n_total % n_procs;
    int deslocamento = 0;

    for (int i = 0; i < n_procs; i++) {
        counts[i] = base + (i < resto ? 1 : 0);
        displs[i] = deslocamento;
        deslocamento += counts[i];
    }
}

void trocar_halo(const float *bloco, int n, int raio, int rank, int n_procs,
                 float *estendido) {
    // Zera primeiro: as extremidades globais ja ficam com halo de zeros
    memset(estendido, 0, (n + 2 * raio) * sizeof(float));
    memcpy(estendido + raio, bloco, n * sizeof(float));

    // Nas pontas, MPI_PROC_NULL faz o Sendrecv virar operacao nula e o halo
    // daquele lado continua zerado
    int esquerda = (rank == 0)            ? MPI_PROC_NULL : rank - 1;
    int direita  = (rank == n_procs - 1)  ? MPI_PROC_NULL : rank + 1;

    MPI_Status status;

    // Envia as primeiras 'raio' amostras para a esquerda e recebe da direita o
    // meu halo direito
    MPI_Sendrecv(estendido + raio,            raio, MPI_FLOAT, esquerda, 0,
                 estendido + raio + n,        raio, MPI_FLOAT, direita, 0,
                 MPI_COMM_WORLD, &status);

    // Envia as ultimas 'raio' amostras para a direita e recebe da esquerda o
    // meu halo esquerdo
    MPI_Sendrecv(estendido + n,               raio, MPI_FLOAT, direita, 1,
                 estendido,                   raio, MPI_FLOAT, esquerda, 1,
                 MPI_COMM_WORLD, &status);
}
