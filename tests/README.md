# Testes do TricBar

    python3 tests/run_tests.py            # contra este repositorio
    python3 tests/run_tests.py <pasta>    # contra outra copia (ex.: a versao original)

Requisitos: Python 3. O teste "(g++)" também precisa de g++ (Linux/macOS/MSYS2); sem ele, falha avisando.

## O que cobre
- Regressão das correções (Engine, Launcher, Installer, WebSetup, hook): lê o código-fonte real.
- Consistência do `mode=` entre Engine, hook e Launcher (tabela canônica).
- Lógica pura do WebSetup (escape de aspas para PowerShell, detecção do .NET 8) compilada com g++
  a partir do trecho literal do arquivo.
- Integridade: finais de linha e balanceamento de chaves/parênteses.

## O que NÃO cobre (exige Windows)
Compilação com MSVC/.NET/Windows SDK, injeção no Explorer, efeito visual na barra, UI do Launcher,
fluxo real de instalar/desinstalar. Nenhum desses foi executado. Faça o build completo com
`.\build\buildsetup.ps1` e teste manualmente em Windows 10 e Windows 11 22H2+.

Os testes estáticos são checagens de regressão sobre o texto do código: se o código for refatorado
sem mudar o comportamento, eles podem precisar de ajuste.

# TricBar Tests

python3 tests/run_tests.py            # against this repository
python3 tests/run_tests.py <folder>    # against another copy (e.g., the original version)

Requirements: Python 3. The "(g++)" test also requires g++ (Linux/macOS/MSYS2); if it is missing, the test will fail with a warning.

## What is covered
- Regression testing for fixes (Engine, Launcher, Installer, WebSetup, hook): reads the actual source code.
- Consistency of `mode=` across the Engine, hook, and Launcher (canonical table).
- Core WebSetup logic (escaping quotes for PowerShell, .NET 8 detection), compiled using g++
from the literal code snippets found in the files.
- Integrity: line endings and balanced braces/parentheses.

## What is NOT covered (requires Windows)
Compilation with MSVC/.NET/Windows SDK, Explorer integration, visual effects on the taskbar,
Launcher UI, and the actual installation/uninstallation workflow. None of these are executed.
Perform a full build using `.\build\buildsetup.ps1` and test manually on Windows 10
and Windows 11 22H2+.

These static tests serve as regression checks on the source code text; if the code is refactored
without changing its behavior, the tests may require adjustments.