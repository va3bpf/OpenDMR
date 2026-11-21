# Guía de instalación de OpenDMR

Reemplazar el firmware de fábrica del **Baofeng DM-32 / DM-32UV** con OpenDMR, actualizarlo posteriormente y volver al firmware original. English: [installation.en.md](installation.en.md) | 中文：[installation.md](installation.md). Uso del radio tras la instalación: [manual de usuario](manual.es.md).

> ⚠️ **Flashear el firmware conlleva riesgos. Lo haces bajo tu propia responsabilidad.**  
> **El primer paso obligatorio es la copia de seguridad (backup)**: la memoria flash de datos contiene **la calibración única de transmisión y recepción de este radio en particular**, la cual no puede recuperarse desde otro equipo.  
> Con una copia de seguridad todo se puede restaurar, incluso tras una escritura fallida: el chip cuenta con un bootloader en memoria ROM, por lo que una memoria de programa dañada siempre puede volver a escribirse.

## Contenido

1. [Requisitos](#1-requisitos)
2. [Copia de seguridad (Obligatorio)](#2-copia-de-seguridad-obligatorio)
3. [Escribir el firmware](#3-escribir-el-firmware)
4. [Primer encendido](#4-primer-encendido)
5. [Escribir el codeplug](#5-escribir-el-codeplug)
6. [Actualizaciones](#6-actualizaciones)
7. [Volver al firmware de fábrica](#7-volver-al-firmware-de-fábrica)
8. [Solución de problemas](#8-solución-de-problemas)

---

## 1. Requisitos

### El paquete de lanzamiento (Release)

Descarga **`OpenDMR-<versión>.zip`** desde la página de Releases y descomprímelo en una carpeta (ejecuta todos los comandos descritos a continuación dentro de dicha carpeta). Contiene:

| Archivo | Para qué sirve |
|---|---|
| `opendmr.bin` | El firmware principal |
| `opendmr_BFUV32.bin` | El mismo firmware con la cabecera requerida por el modo de actualización de fábrica (solo para casos especiales, ver [7](#7-volver-al-firmware-de-fábrica)) |
| `languages/` | Los idiomas de la interfaz (`zh.json` chino, `es.json` español) y la fuente china `cjk.bin`; se escriben en el radio desde la página Idiomas de OpenDMR CPS, ver [5](#5-escribir-el-codeplug) |
| `tools/backup/dm32_dump.py`, `tools/backup/dumper/dumper.bin` | Herramienta de volcado / escritura (a través del bootloader del chip) |
| `tools/fonts/ttf/RobotoCondensed-VF.ttf` | Fuente tipográfica utilizada por dicha herramienta para mostrar texto en la pantalla del radio |
| `tools/debug/fw_update.py`, `tools/debug/updater/updater.bin` | Actualizaciones posteriores (mientras el radio ya ejecuta OpenDMR) |
| `tools/debug/pin_wipe.py` | Borra un PIN de encendido o bloqueo olvidado (con el radio apagado, ejecutar y encender antes de 30 s; mantiene el codeplug y los ajustes) |
| `tools/ids/id_db.py` | Compila y escribe la base de datos de IDs de DMR con ubicaciones (manual de usuario, sección 13) |
| `docs/manual.md`, `docs/manual.en.md`, `docs/manual.es.md`, `docs/manual/` | Manual de usuario |
| `opendmr-cps.html` | Nuestro software de gestión de codeplug (OpenDMR CPS): abrir con doble clic en Chrome o Edge; ver [5](#5-escribir-el-codeplug) |
| `docs/installation.md`, `docs/installation.en.md`, `docs/installation.es.md`, `README.md`, `README.en.md` | Esta guía de instalación y el archivo léame del proyecto |
| `licences/` | Licencias OFL de las fuentes (Roboto Condensed, Noto Sans SC) |
| `VERSION.txt` | Versión, commit de git y fecha/hora de empaquetado |

Un archivo adicional, **`SHA256SUMS.txt`**, contiene las sumas de verificación criptográficas (puedes verificarlo con `certutil -hashfile OpenDMR-<versión>.zip SHA256` en Windows o `shasum -a 256` en Mac/Linux).

Para gestionar el codeplug, puedes usar **OpenDMR CPS** (`opendmr-cps.html` dentro del archivo comprimido o descargado de la página de Releases) o el **OpenGD77 CPS** (no incluido en la descarga: descargable desde <https://www.opengd77.com/>, en una versión compatible con DM32/UV008; probado con E2026.07.13.01). Consulta la sección [5](#5-escribir-el-codeplug).

### Computadora

- Windows 10 / 11, macOS o Linux.
- **Python 3** (<https://www.python.org/downloads/>; en Windows asegúrate de marcar "Add python.exe to PATH" en el instalador). Luego, en una terminal o PowerShell:

  ```powershell
  py -m pip install pyserial pillow
  ```
  *(En macOS/Linux: `python3 -m pip install pyserial pillow`)*

  *(pillow es opcional: con él instalado, la pantalla del radio muestra texto explicativo además de la barra de progreso).*

### Cable de programación

- El cable con chip CH340 incluido habitualmente con el radio funciona; un cable con chip FTDI (por ejemplo, BTECH PC03) es más estable y fiable.
- Al conectarlo, el Administrador de dispositivos de Windows → Puertos (COM y LPT) indicará su puerto COM (los comandos siguientes asumen `COM6`: cámbialo por el tuyo, o `/dev/cu.usbserial-...` en Mac/Linux). Instala el controlador CH340 si el sistema no lo reconoce.
- Conecta el conector firmemente hasta el fondo: la toma del DM-32 es bastante ajustada y, si no entra completamente, la comunicación fallará.

### Batería

Carga la batería completamente antes de comenzar. El proceso completo (principalmente la copia de seguridad) toma alrededor de 40 minutos.

---

## 2. Copia de seguridad (Obligatorio)

El DM-32 posee dos chips de memoria flash:

| Chip | Contenido | Tiempo de volcado |
|---|---|---|
| Flash de programa (25Q08 o 25Q16, 1 – 2 MB) | Bootloader y firmware de fábrica | Alrededor de 1 minuto |
| Flash de datos (25Q128, 16 MB) | Codeplug original, fuentes, mensajes de voz y **calibración** | Alrededor de 13 minutos |

**Lee cada chip dos veces**: solo dos lecturas 100% idénticas garantizan un respaldo seguro.

Cada lectura se realiza con el mismo procedimiento:

1. **Apaga el radio** y conecta el cable de programación;
2. Ejecuta el comando; la terminal indicará `switch the radio on now (within 30 s)`;
3. **Enciende el radio antes de que pasen 30 s, sin pulsar ninguna tecla.** Es normal que la pantalla se quede en negro o muestre una barra de progreso y que el LED quede apagado;
4. Espera a que el comando termine (mostrará los bytes y segundos transcurridos), y **apaga el radio** antes de la siguiente ejecución.

```powershell
mkdir backup
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin
py -u tools\backup\dm32_dump.py COM6 program backup\program-2.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-2.bin
```

Compara los dos volcados de cada memoria:

En Windows:
```powershell
fc.exe /b backup\program-1.bin backup\program-2.bin
fc.exe /b backup\data-1.bin backup\data-2.bin
```

En macOS / Linux:
```bash
cmp backup/program-1.bin backup/program-2.bin
cmp backup/data-1.bin backup/data-2.bin
```

Ambos comandos deben reportar que **no se encontraron diferencias**. Si hubiera diferencias, repite la lectura hasta obtener dos archivos idénticos.

Guarda la carpeta `backup` en un lugar seguro (nube, memoria USB) y **etiquétala con el número de serie de tu radio**: cada copia de seguridad es exclusiva e intransferible de ese equipo.

---

## 3. Escribir el firmware

El proceso es idéntico al de la copia de seguridad: radio apagado, ejecutar comando, encender antes de 30 segundos, sin tocar ninguna tecla. El parámetro `--write` escribe únicamente los sectores de 4 KB que difieren del archivo, los verifica releyéndolos y realiza una comprobación final completa.

```powershell
py -u tools\backup\dm32_dump.py COM6 program opendmr.bin --write --start 0xC000
```

*(El bootloader, por debajo de `0xC000`, nunca se sobreescribe).* Debe finalizar indicando `... sectors written, all ... the same as the file`. Apaga el radio.

---

## 4. Primer encendido

Al encender el radio, iniciará en la pantalla principal de canal. Como aún no tiene ajustes personalizados, utilizará los valores por defecto; el codeplug estará vacío hasta que lo grabes en el siguiente paso.

Recomendaciones iniciales:

- **Idioma**: mantén pulsada la tecla de menú (la tecla izquierda debajo de la pantalla) para entrar al menú principal → Options (Opciones) → Language (Idioma); pulsa la tecla de menú para alternar entre **Español** / English / 中文;
- **Tu ID de DMR**: Opciones → Opciones generales → ID usuario DMR (o configúralo desde el CPS);
- **Fecha y hora**: Menú principal → Info radio → Hora / Fecha (si el GPS obtiene fijación, el reloj se sincroniza automáticamente; ver sección "GPS" en el manual).

Para conocer las teclas y pantallas en detalle, consulta el [manual de usuario](manual.es.md).

---

## 5. Escribir el codeplug

Puedes utilizar cualquiera de los dos programas; el formato de codeplug es compatible, por lo que pueden alternarse:

| Característica | OpenDMR CPS (`opendmr-cps.html` en la descarga) | OpenGD77 CPS |
|---|---|---|
| Instalación | Ninguna: abrir con doble clic en Chrome / Edge | Instalador para Windows |
| Nombres en chino | Editables, escritos en GB2312, mostrados en el radio | No probado |
| Canales P25 y NAC, AM / USB / LSB | Editables nativamente | P25 se muestra como DMR, AM/SSB como analógico: se pierden al editar |
| Ajustes del radio (potencias, brillo, ganancia de micro, etc.) | Se leen y escriben directamente | No se leen ni se escriben |
| Base de datos de DMR ID (nombres de corresponsales) | Próximamente | Compatible |

**OpenDMR CPS**:

1. Abre `opendmr-cps.html` en Google Chrome o Microsoft Edge;
2. Enciende el radio en **modo normal** (ejecutando OpenDMR), conecta el cable, pulsa "Conectar" y selecciona el puerto serie correspondiente;
3. "Leer radio" → realiza tus modificaciones → "Escribir radio"; el equipo se reiniciará automáticamente al terminar. Haz clic en "Guardar archivo" antes para conservar una copia de seguridad.

**OpenGD77 CPS**:

1. Descarga el OpenGD77 CPS desde <https://www.opengd77.com/> (versión con soporte DM32/UV008; probado con E2026.07.13.01) e instálalo;
2. Inicia el OpenGD77 CPS y selecciona en modelo de radio: **Radio type DM32/UV008**;
3. Enciende el radio en **modo normal** y conecta el cable;
4. Primero haz **Leer codeplug**, edita los canales, zonas, contactos, listas de TG, y luego selecciona **Escribir codeplug**.

Durante la lectura o escritura, la pantalla del radio indicará la sección y el porcentaje completado: no desconectes el cable durante el proceso.

- El CPS oficial de fábrica y sus archivos de codeplug **no son compatibles**: es necesario recrear el codeplug.
- OpenGD77 CPS también permite cargar la base de datos de IDs de DMR (nombres de radioaficionados) y los elementos orbitales de satélites; sin embargo, no gestiona los ajustes internos del radio: modifícalos directamente desde el menú del equipo o mediante OpenDMR CPS.

---

### Idiomas de la interfaz

El firmware solo trae inglés. Para la interfaz en español o chino: en OpenDMR CPS conecta el radio, abre la página
Idiomas y pulsa Elegir carpeta: la carpeta `languages` del zip. Chino y español quedan marcados; pulsa Escribir en la
radio (el chino con su fuente tarda unos 2 minutos). Después elígelo en Opciones -> Language del radio. Conviene
escribir el chino aunque uses otro idioma: los caracteres chinos de los nombres de canales y contactos necesitan su
fuente.

---

## 6. Actualizaciones

Una vez que OpenDMR ya está instalado en el equipo, actualizar a una nueva versión no requiere ciclos de apagado ni tiempos de espera estrictos: enciende el radio de forma normal, conecta el cable y ejecuta:

En Windows:
```powershell
py tools\debug\fw_update.py opendmr.bin --port COM6
```

En macOS / Linux:
```bash
python3 tools/debug/fw_update.py opendmr.bin --port /dev/cu.usbserial-xxxx
```

Solo se sobreescribirán los sectores modificados (alrededor de 35 s si cambia por completo), la pantalla mostrará una barra de progreso y el radio se reiniciará automáticamente. Todos tus ajustes y el codeplug se conservan intactos.

Si vienes de la versión 0.7.0 o anterior: los idiomas y las fuentes tienen un formato nuevo, así que tras la
actualización la interfaz queda en inglés y los nombres en chino no se ven: escríbelos una vez con el CPS (sección 5,
Idiomas de la interfaz). Haz lo mismo cuando una versión cambie un idioma; lo que el radio ya tiene igual no se
vuelve a escribir.

Si por alguna razón `fw_update.py` fallara o el equipo no arrancase, simplemente vuelve a escribir `opendmr.bin` siguiendo el procedimiento de la sección 3 (con el radio apagado y encendiéndolo en los primeros 30 segundos).

---

## 7. Volver al firmware de fábrica

Para regresar al estado original, restaura la copia de seguridad **de este radio** (radio apagado, ejecutar comando, encender en 30 s):

```powershell
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin --write
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin --write
```

El parámetro `--write` únicamente escribe los sectores que presenten diferencias. Una vez restaurados ambos chips, el radio quedará exactamente como estaba en el momento del respaldo (firmware original, codeplug, avisos de voz y calibración de fábrica).

Si únicamente deseas volver al firmware de fábrica manteniendo intacta la memoria de datos: mantén pulsadas las teclas **PTT + SK1** al encender el equipo (modo de actualización oficial de fábrica) y carga el firmware oficial utilizando la herramienta de actualización del CPS original de Baofeng.

Ten presente que lo que OpenDMR haya escrito en la memoria de datos (fuentes, ajustes, codeplug) permanecerá allí y podría haber sobreescrito información del firmware original. Para una restauración 100% limpia y fidedigna, escribe siempre de vuelta el respaldo de la memoria de datos (`data`).

---

## 8. Solución de problemas

**No ocurre nada tras el mensaje `switch the radio on now`, o aparece un error**

- Comienza siempre con el radio **apagado**, enciéndelo justo tras aparecer el mensaje y no presiones ninguna tecla.
- Si un radio con OpenDMR no se apaga girando la perilla, **retira la batería un instante** y vuelve a colocarla.
- Revisa el número de puerto COM y comprueba que el conector del cable esté insertado hasta el fondo.
- Prueba con un cable con chip FTDI (los cables CH340 a veces son sensibles a los tiempos de respuesta del puerto serie).
- Puedes ejecutar `py -u tools\backup\dm32_dump.py COM6 program --speedtest` para verificar la velocidad soportada por tu cable; si hay inestabilidad, añade `--baud 115200` (más lento pero más estable).

**Una escritura fue interrumpida accidentalmente**

No te preocupes: vuelve a ejecutar el mismo comando. Solo se escribirán los sectores faltantes o diferentes. El bootloader reside en la memoria ROM del chip, de modo que una memoria de programa a medio escribir siempre puede volver a flashearse.

**Los textos en caracteres chinos se ven como cajas vacías o no aparecen**

El chino y su fuente faltan o están incompletos: escríbelos con el CPS (sección 5, Idiomas de la interfaz; la fuente `cjk.bin` va con el chino). Los textos que se ven como "??" tienen la misma causa.

**Cómo comprobar la versión instalada**

Menú principal → Info firmware: la primera línea indica la versión (commit de git), seguida de la fecha y hora de compilación.

**Cuánto tiempo conservar la copia de seguridad**

Para siempre. Los valores de calibración fueron ajustados individualmente para tu equipo en los laboratorios de fábrica: sin ellos, la potencia de salida y la sensibilidad de recepción perderán su precisión.
