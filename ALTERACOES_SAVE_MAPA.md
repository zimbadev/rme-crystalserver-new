# Alteracoes No Save Do Mapa

Este arquivo resume as alteracoes feitas para reduzir crash ao salvar mapas grandes e evitar falhas causadas por ponteiros nulos no fluxo de save.

## Arquivo Alterado

- `source/iomap_otbm.cpp`

## Problemas Identificados

### 1. Uso excessivo de memoria ao salvar `.otgz`

Antes, o save de `.otgz` criava todo o `map.otbm` em memoria usando `MemoryNodeFileWriteHandle` e depois escrevia esse bloco no arquivo compactado.

Impacto:

- mapas grandes podiam consumir memoria demais durante o save
- a realocacao do buffer podia crescer varias vezes
- em falha de alocacao, o processo podia encerrar abruptamente

### 2. Loop perigoso durante serializacao de tiles

No fluxo de serializacao da tile, existiam `continue` no meio do processamento quando `ground->getID() == 0`.

Impacto:

- a tile podia nao finalizar corretamente
- o iterador podia nao avancar naquele ciclo
- isso podia causar repeticao infinita, consumo de memoria e crash

### 3. Falhas por ponteiros nulos em estruturas auxiliares

Alguns loops de save assumiam que ponteiros vindos do mapa sempre estavam validos.

Impacto:

- se o mapa tivesse alguma estrutura inconsistente, o editor podia crashar ao salvar

## Alteracoes Aplicadas

### 1. Save de `.otgz` sem duplicar o OTBM inteiro na RAM

O fluxo foi alterado para:

1. salvar o OTBM em um arquivo temporario no disco
2. reabrir esse arquivo para leitura
3. enviar o conteudo para o archive em blocos
4. remover o arquivo temporario ao final

Beneficios:

- evita manter uma segunda copia gigante do mapa em memoria
- reduz risco de crash em mapas grandes
- torna o save compactado mais previsivel

### 2. Remocao dos `continue` perigosos na serializacao da tile

O processamento do `ground` foi ajustado para:

- ignorar itens invalidos sem quebrar o fluxo da tile
- sempre permitir que o no da tile seja fechado corretamente
- sempre permitir que o iterador avance normalmente

### 3. Guardas contra nulos

Foram adicionadas verificacoes defensivas nos saves de:

- conteudo de containers
- itens da tile
- `towns`
- `waypoints`
- `houses`
- `spawnsMonster`
- `spawnsNpc`

Essas verificacoes evitam dereferenciar ponteiros nulos durante o save.

## Resumo Tecnico

### Em containers

- itens nulos, meta items e itens com `ID == 0` passam a ser ignorados no save

### Em tiles

- `ground` com `ID == 0` nao interrompe mais o loop da tile
- itens nulos dentro de `save_tile->items` sao ignorados com seguranca

### Em dados auxiliares do mapa

- entries nulas em `towns`, `waypoints`, `houses`, `spawnMonster` e `spawnNpc` sao ignoradas

## Resultado Esperado

Com essas alteracoes, o editor deve:

- ter menos chance de crashar ao salvar mapas grandes
- suportar melhor save em `.otgz`
- resistir melhor a dados inconsistentes no mapa

## Observacao

Se ainda houver crash em algum mapa especifico, o proximo passo recomendado e adicionar logs detalhados no save para descobrir exatamente qual tile, house, spawn ou item esta corrompido.
