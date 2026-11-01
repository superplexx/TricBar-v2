#!/usr/bin/env python3
"""Testes de regressao do TricBar que rodam fora do Windows (Python 3 + g++ opcional).

Uso:  python3 tests/run_tests.py [raiz_do_projeto]

Sao testes ESTATICOS (leem o codigo-fonte) e de LOGICA (compilam com g++ trechos puros
extraidos literalmente dos arquivos). Nao substituem compilar e testar em Windows.
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
results = []

def read(rel):
    with open(os.path.join(ROOT, rel), 'rb') as f:
        return f.read().decode('utf-8-sig').replace('\r\n', '\n')

def test(name):
    def deco(fn):
        try:
            fn(); results.append((name, 'PASS', ''))
        except AssertionError as e:
            results.append((name, 'FAIL', str(e)))
        except Exception as e:  # erro do proprio teste
            results.append((name, 'ERRO', f'{type(e).__name__}: {e}'))
        return fn
    return deco

# ---------------------------------------------------------------- 1. Engine
@test('Engine: g_tapInjections zera quando o PID do Explorer muda')
def _():
    src = read('Engine/TricBar.cpp')
    m = re.search(r'if \(pid != g_tapPid\)\s*\{(.*?)\}', src, re.S)
    assert m, 'bloco "pid != g_tapPid" nao encontrado'
    assert 'g_tapInjections = 0' in m.group(1), 'contador de injecoes nao e zerado ao trocar de PID'

# ---------------------------------------------------------------- 2. Mapeamento de modos
CANON = {'normal': 'normal', 'blur': 'blur', 'acrylic': 'acrylic', 'macos': 'acrylic',
         'lightliquidglass': 'liquidglass', 'liquidglass': 'liquidglass', 'mica': 'liquidglass',
         'darkliquidglass': 'liquidglass', 'color': 'color'}
DEFAULT = 'clear'
ENGINE_NAMES = {'Normal': 'normal', 'Clear': 'clear', 'Blur': 'blur', 'Acrylic': 'acrylic',
                'LiquidGlass': 'liquidglass', 'Color': 'color'}
HOOK_NUM = {0: 'normal', 1: 'clear', 2: 'blur', 3: 'acrylic', 4: 'liquidglass', 5: 'color', 6: 'liquidglass'}

def engine_table():
    src = read('Engine/TricBar.cpp')
    a = src.index('if (_stricmp(line, "mode") == 0)')
    seg = src[a:src.index('else if (_stricmp(line, "color")', a)]
    table = {}
    for branch in re.split(r'(?=next\.mode = Mode::)', seg):
        m = re.match(r'next\.mode = Mode::(\w+)', branch)
        if m:
            continue
    # percorre ramo a ramo: nomes citados ANTES de cada atribuicao
    for m in re.finditer(r'((?:_stricmp\(equals, "\w+"\) == 0(?:\s*\|\|\s*)?\s*)+)\)\s*next\.mode = Mode::(\w+)|'
                         r'((?:_stricmp\(equals, "\w+"\) == 0(?:\s*\|\|\s*)?\s*)+)\)\s*\{?\s*next\.mode = Mode::(\w+)', seg):
        names = re.findall(r'"(\w+)"', m.group(1) or m.group(3))
        mode = m.group(2) or m.group(4)
        for n in names:
            table[n.lower()] = ENGINE_NAMES[mode]
    return table

def hook_table():
    src = read('NativeHook/TricBar.cpp')
    a = src.index('c.mode = val == "normal"')
    seg = src[a:src.index(';', a)]
    table = {}
    for m in re.finditer(r'((?:val == "\w+"(?:\s*\|\|\s*)?\s*)+)\?\s*(\d)', seg):
        for n in re.findall(r'"(\w+)"', m.group(1)):
            table[n] = HOOK_NUM[int(m.group(2))]
    return table

@test('Modos: Engine e hook interpretam config.ini igual (tabela canonica)')
def _():
    for label, table in (('Engine', engine_table()), ('Hook', hook_table())):
        assert table, f'{label}: nao consegui extrair a tabela de modos'
        for key, want in CANON.items():
            assert table.get(key) == want, f'{label}: mode={key} -> {table.get(key)!r}, esperado {want!r}'

@test('Modos: Launcher le "macos" como acrylic e nao como liquidglass claro')
def _():
    src = read('Launcher/MainWindow.xaml.cs')
    assert re.search(r'mode == "macos"\s*\)\s*\{\s*mode = "acrylic";', src), 'macos nao mapeia para acrylic'
    m = re.search(r'mode is ([^)]*?)\)', src[src.index('mode == "macos"'):])
    assert m and '"macos"' not in m.group(1), 'macos ainda listado entre os modos Liquid Glass'

@test('Modos: Launcher grava apenas valores que Engine e hook conhecem')
def _():
    src = read('Launcher/MainWindow.xaml.cs')
    written = set(re.findall(r'"(\w+)"', src[src.index('ModeKeys = ['):src.index(']', src.index('ModeKeys = ['))]))
    written |= {'lightliquidglass', 'darkliquidglass'}
    for label, table in (('Engine', engine_table()), ('Hook', hook_table())):
        for key in written:
            assert key == 'clear' or key in table, f'{label} nao reconhece mode={key} gravado pelo Launcher'

# ---------------------------------------------------------------- 3. Launcher: dialogos
@test('Launcher: ContentDialog.ShowAsync so e chamado dentro de ShowDialogAsync')
def _():
    src = read('Launcher/MainWindow.xaml.cs')
    assert 'ShowDialogAsync(ContentDialog' in src, 'ShowDialogAsync nao existe'
    body = src[src.index('ShowDialogAsync(ContentDialog'):]
    body = body[:body.index('private async void ShowMessage')]
    assert '_dialogOpen' in body and 'catch' in body, 'sem guarda de reentrada / try-catch'
    outside = src.replace(body, '')
    assert '.ShowAsync()' not in outside, 'ainda ha ShowAsync() direto fora do helper'

# ---------------------------------------------------------------- 4. Instalador
@test('Instalador: script de limpeza tenta varias vezes e nao usa "timeout"')
def _():
    src = read('Installer/Program.cs')
    a = src.index('string script ='); seg = src[a:src.index('File.WriteAllText(scriptPath', a)]
    assert ':retry' in seg and 'goto retry' in seg, 'sem laco de repeticao'
    assert 'ping -n' in seg, 'sem espera por ping'
    assert 'timeout' not in seg, '"timeout" falha sem console de entrada'
    assert re.search(r'rmdir /s /q.*\n.*if not exist', seg), 'nao confere se a pasta sumiu'
    assert 'del /f /q' in seg, 'script nao se autoexclui'

@test('Instalador: arquivos exigidos por ValidateApplicationBundle sao copiados pelos scripts de build')
def _():
    req = re.findall(r'"([\w.]+)"', read('Installer/Program.cs').split('requiredFiles =')[1].split('];')[0])
    req = [r for r in req if '.' in r]
    for script in ('build/buildapp.ps1', 'build/buildappzip.ps1'):
        s = read(script)
        for f in ('TricBar.exe', 'TricBar.dll', 'TricBarConfigurator.pri'):
            assert f in s, f'{script} nao menciona {f}'
        assert 'App.xbf' in s and 'MainWindow.xbf' in s, f'{script} nao valida os .xbf'
    assert {'TricBar.dll', 'TricBarConfigurator.pri', 'App.xbf', 'MainWindow.xbf'} <= set(req)

# ---------------------------------------------------------------- 5. WebSetup nativo
@test('WebSetup: todo caminho interpolado em PowerShell passa pelo escape')
def _():
    src = read('build/WebSetupBootstrap/TricBarWebSetup.cpp')
    assert 'EscapePowerShellSingleQuoted' in src
    for fn in ('bool ExpandZip', 'bool CreateShortcut'):
        a = src.index(fn); body = src[a:src.index('\n    }\n', a)]
        assert body.count('EscapePowerShellSingleQuoted(') >= 2, f'{fn}: argumentos nao escapados'
        for raw in re.findall(r"'\" \+ (\w+) \+", body):
            assert raw in ('destination', 'archivePath', 'shortcutPath', 'targetPath', 'installDir'), raw
        assert 'Raw' in body.split('{')[0], f'{fn}: parametros brutos nao renomeados (risco de usar sem escape)'

@test('WebSetup: atalho da Area de Trabalho nao usa %USERPROFILE%\\Desktop fixo')
def _():
    src = read('build/WebSetupBootstrap/TricBarWebSetup.cpp')
    assert 'USERPROFILE%\\\\Desktop' not in src, 'ainda monta o caminho do Desktop manualmente'
    assert 'CSIDL_DESKTOPDIRECTORY' in src

@test('Hook: blur.log tem limite de tamanho')
def _():
    src = read('NativeHook/TricBar.cpp')
    a = src.index('static void Log('); body = src[a:src.index('static bool ContainsRange', a)]
    assert 'GetFileAttributesExW' in body and 'DeleteFileW' in body and '1u << 20' in body

# ---------------------------------------------------------------- 6. Integridade dos arquivos
@test('Arquivos: finais de linha consistentes (sem LF solto em arquivos CRLF)')
def _():
    bad = []
    for rel in ('Engine/TricBar.cpp', 'Launcher/MainWindow.xaml.cs', 'Installer/Program.cs',
                'build/WebSetupBootstrap/TricBarWebSetup.cpp', 'NativeHook/TricBar.cpp'):
        raw = open(os.path.join(ROOT, rel), 'rb').read()
        crlf = raw.count(b'\r\n'); lone = len(re.findall(rb'(?<!\r)\n', raw))
        if crlf and lone: bad.append(f'{rel} (CRLF={crlf}, LF solto={lone})')
    assert not bad, '; '.join(bad)

@test('Arquivos: chaves/parenteses balanceados nos fontes C++ e C# (sem strings/comentarios)')
def _():
    def strip(s):
        s = re.sub(r'//[^\n]*', '', s)
        s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
        s = re.sub(r'L?"(?:\\.|[^"\\\n])*"', '""', s)
        return re.sub(r"L?'(?:\\.|[^'\\\n])'", "''", s)
    bad = []
    for rel in ('Engine/TricBar.cpp', 'Launcher/MainWindow.xaml.cs', 'Installer/Program.cs',
                'build/WebSetupBootstrap/TricBarWebSetup.cpp', 'NativeHook/TricBar.cpp'):
        s = strip(read(rel))
        for o, c in ('{}', '()'):
            if s.count(o) != s.count(c): bad.append(f'{rel}: {o}{c} {s.count(o)}!={s.count(c)}')
    assert not bad, '; '.join(bad)

# ---------------------------------------------------------------- 7. Logica nativa (g++)
HARNESS = r"""
#include <string>
#include <cstdio>
#include <cwchar>
@@FN@@
static bool DotNet8(const std::wstring& line){ return @@PRED@@ != std::wstring::npos; }
int main(){
  int fail=0;
  auto chk=[&](bool ok,const char*n){ std::printf("    %s  %s\n", ok?"ok":"FALHA", n); if(!ok) fail=1; };
  chk(EscapePowerShellSingleQuoted(L"C:\\Users\\O'Brien\\x.lnk")==L"C:\\Users\\O''Brien\\x.lnk","aspa simples duplicada");
  chk(EscapePowerShellSingleQuoted(L"C:\\Users\\Ana\\x")==L"C:\\Users\\Ana\\x","sem aspas: inalterado");
  chk(EscapePowerShellSingleQuoted(L"''")==L"''''","duas aspas viram quatro");
  chk(EscapePowerShellSingleQuoted(L"")==L"","string vazia");
  chk(EscapePowerShellSingleQuoted(L"d'a'b")==L"d''a''b","varias aspas");
  chk(DotNet8(L"Microsoft.WindowsDesktop.App 8.0.11 [C:\\Program Files\\dotnet\\shared\\Microsoft.WindowsDesktop.App]"),"8.0.11 detectado");
  chk(DotNet8(L"Microsoft.WindowsDesktop.App 8.0.0 [x]"),"8.0.0 detectado");
  chk(!DotNet8(L"Microsoft.WindowsDesktop.App 6.0.36 [C:\\dotnet\\8.0\\shared]"),"6.0 com '8.0' no caminho nao casa");
  chk(!DotNet8(L"Microsoft.WindowsDesktop.App 7.0.20 [C:\\x]"),"7.0 nao casa");
  chk(!DotNet8(L"Microsoft.WindowsDesktop.App 9.0.1 [C:\\x]"),"9.0 sozinho nao conta como 8.0");
  chk(!DotNet8(L"Microsoft.NETCore.App 8.0.11 [C:\\x]"),"NETCore sem WindowsDesktop nao casa");
  return fail;
}
"""

@test('WebSetup (g++): escape de aspas e deteccao do .NET 8 com casos de borda')
def _():
    if not shutil.which('g++'):
        raise AssertionError('g++ indisponivel - teste nao executado')
    src = read('build/WebSetupBootstrap/TricBarWebSetup.cpp')
    a = src.index('    std::wstring EscapePowerShellSingleQuoted'); b = src.index('    bool IsDotNetDesktopRuntimeInstalled')
    loop = src[src.index('while (std::getline(input, line))'):]
    m = re.search(r'if \((line\.find\(L"Microsoft\.WindowsDesktop\.App[^)]*\))\s*!=\s*std::wstring::npos', loop)
    assert m, 'predicado do .NET nao encontrado'
    code = HARNESS.replace('@@FN@@', src[a:b]).replace('@@PRED@@', m.group(1))
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, 't.cpp'), 'w').write(code)
        cc = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', os.path.join(d, 't.cpp'), '-o', os.path.join(d, 't')],
                            capture_output=True, text=True)
        assert cc.returncode == 0, 'falha ao compilar: ' + cc.stderr[:400]
        run = subprocess.run([os.path.join(d, 't')], capture_output=True, text=True)
        assert run.returncode == 0, 'casos falharam:\n' + run.stdout

# ---------------------------------------------------------------- relatorio
w = max(len(n) for n, _, _ in results)
for n, s, msg in results:
    print(f'[{s:4s}] {n}')
    if msg: print('        ->', msg.replace('\n', '\n           '))
bad = [r for r in results if r[1] != 'PASS']
print(f'\n{len(results) - len(bad)}/{len(results)} passaram  (raiz: {ROOT})')
sys.exit(1 if bad else 0)
