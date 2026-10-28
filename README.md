# Palhetador Automático de Guitarra com Arduino e Motor de Passo

Sistema embarcado para testes automatizados de palhetada contínua (*alternate picking*) em cordas de guitarra ou violão, utilizando **Arduino Uno Rev3**, display **DFRobot LCD Keypad Shield** e motor de passo com redução **12V 32-Step (Adafruit 918 / 28BYJ)**.

---

## 1. Visão Geral da Arquitetura

O sistema aciona uma haste horizontal fixada ao eixo do motor, na extremidade da qual fica presa uma palheta. A haste oscila para frente e para trás entre $-\text{Amplitude}$ e $+\text{Amplitude}$ em torno de um ponto central calibrado.

### Principais Recursos
* **Controle de Amplitude em Tempo Real:** Regulagem precisa do ângulo de ataque/curso da palheta através das teclas `UP` e `DOWN`, mesmo durante a execução.
* **Controle de Velocidade / Frequência:** Ajuste de 10 níveis de velocidade pelas teclas `LEFT` e `RIGHT`, com cálculo instantâneo de **SPM** (*Strokes Per Minute* / Palhetadas Por Minuto).
* **Modo de Alinhamento / Calibração (*Jog Center*):** Permite mover a haste passo a passo via botões para posicionar a palheta exatamente sobre a corda desejada antes de travar o ponto zero, **evitando forçar manualmente a caixa de engrenagens do motor**.
* **Arquitetura 100% Não-Bloqueante:** O motor é acionado via temporização em microssegundos (`micros()`), garantindo resposta instantânea aos botões e sem travar a interface.
* **Gerenciamento Térmico (*Coil Release*):** Ao pausar (`STOP`), todas as bobinas são desenergizadas automaticamente, impedindo que o motor de 12V aqueça enquanto oco.
* **Acionamento em Meio-Passo (*Half-Stepping*):** Sequência de 8 fases que reduz a vibração mecânica e ruídos, essencial para um contato limpo e consistente com as cordas.

---

## 2. Hardware e Pinagem

### Componentes Utilizados
1. **Arduino Uno Rev3**
2. **DFRobot LCD Keypad Shield** (Display 16x2 com teclado analógico integrado em `A0`)
3. **Small Reduction Stepper Motor - 12VDC 32-Step** (Adafruit ID 918 / unipolar 5 fios)
4. **Driver de Motor ULN2003** (ou módulo de transistores Darlington equivalente)
5. **Fonte Externa de 12V DC (1A a 2A)**

### Mapa de Pinos do Arduino Uno

| Pino Arduino | Conexão / Função | Observação |
| :--- | :--- | :--- |
| **D4, D5, D6, D7** | Linhas de Dados LCD (DB4-DB7) | Utilizado internamente pelo Shield |
| **D8** | LCD RS (Register Select) | Utilizado internamente pelo Shield |
| **D9** | LCD Enable | Utilizado internamente pelo Shield |
| **D10** | LCD Backlight Control | Utilizado internamente pelo Shield |
| **A0** | Teclado Analógico (5 botões) | Divisor resistivo (Right, Up, Down, Left, Select) |
| **D2** | **Driver ULN2003 - IN1** | Saída digital livre no Shield |
| **D3** | **Driver ULN2003 - IN2** | Saída digital livre no Shield |
| **D11** | **Driver ULN2003 - IN3** | Saída digital livre no Shield |
| **D12** | **Driver ULN2003 - IN4** | Saída digital livre no Shield |
| **GND** | **GND Comum** | Conectar ao GND do Arduino e ao (-) da Fonte 12V |

> [!IMPORTANT]
> **Atenção à Alimentação do Motor:**
> O motor opera a **12V DC** e nunca deve ser alimentado pelo pino de 5V do Arduino. Conecte o terminal positivo (+12V) da fonte externa diretamente ao pino de alimentação do módulo ULN2003. Conecte o negativo (GND) da fonte de 12V ao **GND do Arduino** (terra comum obrigatório para referência de sinal).

---

## 3. Esquema de Ligação

```
  +------------------+                    +-------------------------+
  |  Fonte 12V DC    |--- (+12V) -------->| VCC (ou 5-12V)          |
  |                  |--- (GND) ----------| GND         ULN2003     |
  +------------------+       |            |                         |
                             |            |     Conector 5 pinos    |==== [Motor 12V]
  +------------------+       |            |     (Laranja, Amarelo,  |     (Adafruit 918)
  |  Arduino Uno     |       |            |      Rosa, Azul, Verm)  |
  |  + DFRobot LCD   |       |            |                         |
  |                  |       |            | IN1  IN2   IN3   IN4    |
  |              GND |-------+            +---|----|-----|-----|----+
  |               D2 |------------------------+    |     |     |
  |               D3 |-----------------------------+     |     |
  |              D11 |-----------------------------------+     |
  |              D12 |-----------------------------------------+
  +------------------+
```

---

## 4. Cálculos Mecânicos: Amplitude Angular e Linear

O motor possui **32 passos por revolução** no rotor interno e uma caixa de redução mecânica de **~1/16** (1:16.032).

* **Passos completos por volta do eixo externo:** $32 \times 16 = 512\text{ passos/revolução}$
* **Meio-passos (*Half-steps*) por volta:** $512 \times 2 = 1024\text{ meio-passos/revolução}$
* **Resolução angular por meio-passo:**
  $$\theta_{\text{passo}} = \frac{360^\circ}{1024} \approx 0.3516^\circ$$

### Como calcular o curso linear na ponta da palheta:
Se a haste horizontal tiver comprimento $L$ (em milímetros, medido do centro do eixo do motor até a ponta da palheta) e a amplitude configurada for de $\pm A$ passos:

1. **Ângulo total de excursão ($\Delta\theta$):**
   $$\Delta\theta = 2 \times A \times 0.3516^\circ$$
2. **Deslocamento linear total do arco ($\Delta s$):**
   $$\Delta s \approx L \times \left(\Delta\theta \times \frac{\pi}{180^\circ}\right)$$

#### Exemplo Prático:
Para uma haste de $L = 50\text{ mm}$ com amplitude de $A = 30\text{ passos}$ ($\pm 10.5^\circ$, curso total de $21^\circ$):
$$\Delta s \approx 50\text{ mm} \times \left(21^\circ \times \frac{\pi}{180}\right) \approx 18.3\text{ mm}$$
*(Ideal para palhetar amplamente uma corda ou realizar varredura de duas cordas adjacentes).*

---

## 5. Como Operar o Sistema

O display LCD de 16x2 fornece leitura contínua e status do sistema:

### Telas do Display

#### 1. Modo Normal (`[RUN]` ou `[STOP]`)
```
+----------------+
|[RUN]  V:5 180SPM| -> Linha 0: Status, Nível de Vel (1-10) e Frequência (SPM)
|A:+-30p  10.5°  | -> Linha 1: Amplitude em passos e graus equivalentes
+----------------+
```

#### 2. Modo de Alinhamento Manual (`[AJUSTE CENTRO]`)
```
+----------------+
|[AJUSTE CENTRO] | -> Modo de ajuste da posição de repouso
|Pos:  +4  UP/DWN| -> Posição de deslocamento atual
+----------------+
```

### Funções dos Botões

| Botão | No Modo `RUN` | No Modo `STOP` | No Modo `AJUSTE CENTRO` |
| :--- | :--- | :--- | :--- |
| **SELECT** (Clique Curto) | Para o motor e volta ao centro (`STOP`) | Inicia a oscilação da palheta (`RUN`) | Salva a posição atual como o novo ponto zero e sai para `STOP` |
| **SELECT** (Segurar 1.2s) | — | Entra no modo de **Alinhamento Manual** | Salva o novo centro e sai para `STOP` |
| **UP** | Aumenta a amplitude (+2 passos) | Aumenta a amplitude (+2 passos) | Move a palheta 1 passo no sentido horário (CW) |
| **DOWN** | Diminui a amplitude (-2 passos) | Diminui a amplitude (-2 passos) | Move a palheta 1 passo no sentido anti-horário (CCW) |
| **RIGHT** | Aumenta velocidade (níveis 1 a 10) | Aumenta velocidade | — |
| **LEFT** | Diminui velocidade | Diminui velocidade | — |

> [!TIP]
> **Como alinhar a palheta na primeira montagem:**
> 1. Encaixe a haste no eixo do motor sem forçar a rotação.
> 2. Ligue o Arduino (ele iniciará em `[STOP]`).
> 3. Segure o botão **`SELECT`** por mais de 1 segundo até aparecer `[AJUSTE CENTRO]`.
> 4. Use **`UP`** ou **`DOWN`** para mover suavemente a palheta até que ela fique exatamente centralizada sobre a corda da guitarra.
> 5. Pressione **`SELECT`** brevemente. A posição atual é gravada como o **ponto zero de repouso**.
> 6. Pressione **`SELECT`** novamente para iniciar o teste de palhetada!

---

## 6. Dicas para Testes com Cordas de Instrumento

1. **Flexibilidade da Palheta:** Use palhetas médias ou finas (0.60mm a 0.88mm) para os primeiros testes. Palhetas excessivamente rígidas (acima de 1.5mm) podem transferir choque mecânico excessivo ao redutor do motor caso o curso esteja muito profundo.
2. **Ângulo de Ataque:** Fixe a palheta com uma leve inclinação angular (~5° a 10° em relação à vertical da corda) para que ela deslize suavemente pela corda na ida e na volta, simulando a técnica natural de palhetada alternada humana.
3. **Ajuste Fino da Amplitude:** Inicie o teste com amplitude baixa (ex: $\pm 10$ passos) com o motor posicionado próximo à corda e aumente gradualmente usando o botão `UP` até ouvir o som limpo do ataque da palheta.
