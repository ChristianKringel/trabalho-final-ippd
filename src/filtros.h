#ifndef FILTROS_H
#define FILTROS_H

// Filtros de audio aplicados sobre o bloco local de cada processo, com o laco
// paralelizado por OpenMP. Os ponto a ponto (ganho, threshold, normalizacao)
// nao dependem de vizinhos; os de vizinhanca (convolucao, passa-alta, eco)
// recebem o vetor 'estendido' com halo montado por trocar_halo.
// O parametro rank so serve para o prefixo dos logs.

// Liga os logs por thread (uma linha por thread por filtro). Desligados por
// padrao: saem de dentro da regiao paralela e contaminam a medicao de tempo.
// Ligue com --verbose para demonstrar a divisao do trabalho, nao para medir.
void filtros_set_verbose(int ativo);

// Multiplica cada amostra por um ganho fixo (>1 amplia, <1 reduz). O clipping
// so acontece na hora de escrever o .wav.
void aplicar_filtro_ganho(float *bloco, int n, float ganho, int rank);

// Zera amostras com |valor| abaixo do limiar. Denoising simples no dominio do
// tempo: corta o ruido de fundo de baixa energia e deixa passar o sinal forte.
void aplicar_threshold_ruido(float *bloco, int n, float limiar, int rank);

// Passa-baixa por media movel: cada saida e a media das (2*raio + 1) amostras
// centradas nela. Recebe 'estendido' com (n + 2*raio) amostras no formato
// [halo esq (raio) | bloco (n) | halo dir (raio)] e escreve n valores em saida.
void aplicar_convolucao_media(const float *estendido, float *saida, int n,
                              int raio, int rank);

// Passa-alta: original menos a media movel. A media guarda a parte lenta
// (graves), o que sobra e a parte rapida (agudos) -- atenua ronco de vento.
// Mesmo formato de 'estendido' do passa-baixa.
void aplicar_passa_alta_media(const float *estendido, float *saida, int n,
                              int raio, int rank);

// Eco (delay): saida[i] = x[i] + atenuacao * x[i - atraso]. So olha para tras,
// entao a troca de halo usa raio = atraso (o halo direito nao e usado, mas o
// formato e mantido para reaproveitar trocar_halo).
void aplicar_eco(const float *estendido, float *saida, int n, int atraso,
                 float atenuacao, int rank);

// Normalizacao de pico, em tres etapas:
//   1. cada processo calcula o pico do seu bloco (reducao OpenMP);
//   2. o main junta tudo num pico global com MPI_Allreduce(MAX) e calcula
//      fator = alvo / pico_global;
//   3. cada processo multiplica o bloco por esse fator.
// Como o fator e igual em todos, a saida nao depende do numero de processos.
float pico_local_bloco(const float *bloco, int n);
void aplicar_normalizacao(float *bloco, int n, float fator, int rank);

#endif // FILTROS_H
