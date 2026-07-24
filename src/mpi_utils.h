#ifndef MPI_UTILS_H
#define MPI_UTILS_H

// Utilitarios da parte MPI: particao do vetor de amostras em blocos contiguos
// (um por processo) e troca de bordas entre vizinhos.

// Divide n_total elementos entre n_procs processos, preenchendo counts (quantos
// cada um recebe) e displs (offset do bloco no vetor global). O resto vai de um
// em um para os primeiros processos, logo os blocos diferem em no maximo 1.
// counts e displs precisam ter n_procs posicoes. Aritmetica pura, sem MPI.
void calcular_particao(int n_total, int n_procs, int *counts, int *displs);

// Monta 'estendido' (n + 2*raio) para os filtros de vizinhanca: bloco local no
// meio, 'raio' amostras de cada vizinho nas pontas, via MPI_Sendrecv. Nas
// extremidades globais nao ha vizinho e o halo fica zerado.
// Requisito: cada bloco tem no minimo 'raio' amostras (o chamador garante).
void trocar_halo(const float *bloco, int n, int raio, int rank, int n_procs,
                 float *estendido);

#endif // MPI_UTILS_H
