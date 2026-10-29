# Kairo

Navegador para Windows en C++ sobre **CEF (Chromium Embedded Framework)**, con bloqueador de anuncios
y cliente de proxy/VPN integrados en el código, y una interfaz propia totalmente personalizable.

> Estado: **primer borrador sin compilar**. Lo escribí sin acceso a CEF ni a un compilador, así que
> es probable que haya que ajustar alguna firma de CEF (ver "Si no compila").

## Qué incluye

- **Interfaz propia** (HTML/CSS/JS en `ui/`): pestañas en píldora, omnibox, temas (Aurora, Brasa, Papel,
  Terminal), color de acento, redondeo, densidad y buscador configurables. Los ajustes se guardan solos.
- **Pestañas**: cada una es un `CefBrowserView`; las ocultas quedan limitadas por Chromium (menos CPU/RAM).
  Los popups y `target=_blank` se abren como pestaña. Atajos: Ctrl+T, Ctrl+W, Ctrl+L, Ctrl+R, Ctrl+Tab.
- **Bloqueador de anuncios** (`src/adblock.cc`): filtra en `OnBeforeResourceLoad` por dominio (host y
  dominios padre) con un `unordered_set`, O(1) por petición. Lee todos los `.txt` de `ui/filters/`.
  Además envía `Sec-GPC: 1` (Global Privacy Control).
- **Proxy/VPN** (`Shell::SetProxy`): SOCKS5/SOCKS4/HTTP(S) para todo el navegador, sin fallback a conexión
  directa si el proxy cae, y WebRTC restringido para no filtrar la IP real.
- **Rendimiento**: raster por GPU, zero-copy, sin red ni sync en segundo plano (`src/app.cc`).
- **Seguridad**: solo la barra de interfaz tiene el puente `cefQuery`; las páginas web no pueden enviar
  comandos al navegador, y la interfaz no puede navegar a sitios web.

## Compilar (Windows)

Requisitos: Visual Studio 2022 (carga de trabajo C++), CMake 3.21+, Git.

```bat
git clone https://github.com/chromiumembedded/cef-project.git
cd cef-project
:: copia aquí la carpeta "kairo" de este proyecto
```

En el `CMakeLists.txt` raíz de `cef-project`, junto a los demás `add_subdirectory(...)` de aplicaciones,
añade:

```cmake
add_subdirectory(kairo)
```

Luego:

```bat
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release --target kairo
```

El ejecutable queda en `build\kairo\Release\kairo.exe` con las DLL de CEF y la carpeta `ui\` al lado.

## Personalizar

- Temas y buscadores: constantes `THEMES` y `ENGINES` al inicio del script de `ui/index.html`.
- Aspecto: todo el cromo del navegador usa variables CSS (`--bg`, `--panel`, `--accent`, `--r`...).
- Más anuncios bloqueados: suelta listas en formato hosts en `ui/filters/`.

## Límites conocidos

- El adblock es **por dominio**: no hace filtrado cosmético (ocultar huecos) ni reglas de EasyList con
  rutas/opciones. Para eso habría que integrar `adblock-rust`.
- El "VPN" es un **proxy** que tú aportas (servidor SOCKS5/HTTP). Un túnel de sistema (WireGuard) sería
  un módulo aparte con permisos de administrador.
- Sin sandbox de CEF (`no_sandbox = true`) para simplificar el build.
- Ventana con marco normal de Windows; sin marcadores, historial, descargas ni gestor de contraseñas.

## Si no compila

- `GetBrowserRuntimeStyle` / `GetWindowRuntimeStyle` / `CEF_RUNTIME_STYLE_ALLOY`: existen en CEF recientes.
  Si tu versión no los tiene, borra esos overrides en `src/shell.cc`.
- `OnBeforePopup`: la firma cambia entre versiones (parámetro `popup_id`). Copia la de
  `include/cef_life_span_handler.h` de tu CEF.
- Si algo más falla, pega el error y lo ajusto.
