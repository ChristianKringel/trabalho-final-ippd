#!/usr/bin/env python3
"""Gera .wav sinteticos (seno + ruido) para testar o processamento.

Serve para ter audios de tamanhos variados, exercitando a divisao em blocos com
numeros diferentes de processos MPI, e para conferir o calculo de frequencia
dominante (a frequencia do seno e conhecida). Usa so a stdlib (modulo wave).

Exemplos:

    python3 scripts/gerar_wav_teste.py --preset
    python3 scripts/gerar_wav_teste.py --duracao 5 --freq 440 --ruido 0.05 \
        --saida data/entrada/medio.wav
"""

import argparse
import math
import os
import random
import struct
import wave


def gerar(caminho, duracao, sample_rate, freq, amplitude, ruido, semente):
    """Escreve um .wav mono de 16 bits com um seno mais ruido gaussiano leve."""
    rng = random.Random(semente)
    num_amostras = int(round(duracao * sample_rate))

    quadros = bytearray()
    for i in range(num_amostras):
        t = i / sample_rate
        valor = amplitude * math.sin(2.0 * math.pi * freq * t)
        if ruido > 0.0:
            valor += rng.gauss(0.0, ruido)
        if valor > 1.0:
            valor = 1.0
        elif valor < -1.0:
            valor = -1.0
        inteiro = int(valor * 32767.0)
        quadros += struct.pack("<h", inteiro)

    pasta = os.path.dirname(caminho)
    if pasta:
        os.makedirs(pasta, exist_ok=True)

    with wave.open(caminho, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(bytes(quadros))

    print("gerado %s: %d amostras, %.2f s, seno de %.1f Hz, ruido %.3f" %
          (caminho, num_amostras, duracao, freq, ruido))


def main():
    p = argparse.ArgumentParser(description="Gera audios .wav sinteticos de teste.")
    p.add_argument("--duracao", type=float, default=5.0, help="duracao em segundos")
    p.add_argument("--sample-rate", type=int, default=44100, help="taxa de amostragem em Hz")
    p.add_argument("--freq", type=float, default=440.0, help="frequencia do seno em Hz")
    p.add_argument("--amplitude", type=float, default=0.8, help="amplitude do seno em [0, 1]")
    p.add_argument("--ruido", type=float, default=0.05, help="desvio padrao do ruido em [0, 1]")
    p.add_argument("--semente", type=int, default=1, help="semente do gerador de ruido")
    p.add_argument("--saida", type=str, default="data/entrada/audio.wav", help="caminho de saida")
    p.add_argument("--preset", action="store_true",
                   help="gera um conjunto curto/medio/longo em data/entrada/")
    args = p.parse_args()

    if args.preset:
        # tamanhos escolhidos para nao dividirem exato por 2, 4 ou 8, forcando
        # o Scatterv a lidar com blocos desiguais
        gerar("data/entrada/curto.wav", 0.5, args.sample_rate, 440.0, 0.8, 0.05, 1)
        gerar("data/entrada/medio.wav", 5.0, args.sample_rate, 440.0, 0.8, 0.05, 2)
        gerar("data/entrada/longo.wav", 30.0, args.sample_rate, 1000.0, 0.7, 0.05, 3)
    else:
        gerar(args.saida, args.duracao, args.sample_rate, args.freq,
              args.amplitude, args.ruido, args.semente)


if __name__ == "__main__":
    main()
