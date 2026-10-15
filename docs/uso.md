# Uso

Telegarrm se maneja desde el navegador (`http://<ip-del-equipo>:8080`), también desde el móvil. Esta guía explica cada pestaña. Para instalarlo, mira el [README](../README.md).

## Qué canales entiende
Telegarrm trabaja con los canales y grupos (también los grupos con temas) en los que publican series y películas como archivos. Reconoce:
- **Fichas**: una foto con un texto con el título, el año, la calidad, la sinopsis… Los archivos que van después son de esa obra.
- **Nombres de episodio**: `1x01`, `S01E01`, `T1E3`, `Temporada 1 Capítulo 3` y, en los canales de anime, `Serie - 01`, `Serie S2 - 08` o `Serie T2 - 01`. Los canales de anime se detectan solos.
- **Archivos troceados** (`.zip.001`, `.part01.rar`…): todas las partes forman un solo archivo, que se descomprime al terminar la descarga.
- **Versiones**: la misma película o el mismo episodio en 1080p, 4K HDR, REMUX… aparecen juntos, como versiones.

## Catálogo
Todas las series y películas de los canales que vigilas, con portada, año, calidades disponibles y si están en emisión.
- **Buscar y filtrar**: por título, género o canal; por tipo (series, películas, en emisión, en seguimiento) y por canal. Se puede ordenar por título o por novedades.
- **«Añadidas recientemente»**: arriba, lo último que se ha publicado.
- **La misma obra en varios canales**: sale una sola vez. Telegarrm la reconoce por su ficha de TMDB, aunque se llame distinto en cada canal (por ejemplo, *Boku no Hero Academia* y *My Hero Academia*).

### Ficha de una obra
- **Datos**: sinopsis, géneros, idiomas, enlaces a TMDB e IMDb y la ficha original de Telegram.
- **Canales**: si la obra está en varios, arriba se elige de cuál ver los archivos. Cada canal tiene los suyos, y lo que descargues de cualquiera va a la misma carpeta de tu biblioteca.
- **Episodios** (series), por temporada, con el título de cada uno y sus versiones. Lo que ya tienes en la biblioteca aparece marcado.
- **Versiones** (películas), de mejor a peor calidad.
- **Calidad real**: el botón «?» de una versión lee el principio del archivo y dice su resolución y si tiene HDR, sin descargarlo entero. «Comprobar calidades» lo hace con una temporada entera. Es útil porque los nombres a veces no dicen la verdad.
- **Se actualiza sola**: si se publica algo nuevo de la obra mientras la miras, aparece un aviso y la fila nueva se ilumina.

## Descargas
- **Una versión**: «Almacenar en disco» junto a cada versión.
- **Una temporada**: encima de cada temporada hay un botón por versión («Almacenar en 1080p (12 GB)»), que pone en cola los episodios de esa versión que no tengas ya descargados ni en la cola.
- **La serie completa**: en la ficha de una serie, Telegarrm cuenta cuántos episodios tienes y ofrece descargar los que faltan (o la serie entera), en la mejor versión de cada uno y hasta la calidad máxima que elijas.

Las descargas van de una en una. La pestaña *Descargas* muestra el progreso, la velocidad y el tiempo restante, y permite cancelar o reintentar. Al terminar, cada descarga:
1. se descomprime si hace falta;
2. comprueba la calidad real del vídeo;
3. se renombra y se lleva a la biblioteca: `Película (Año) [tmdbid-N]/Película (Año) - 1080p.mkv` o `Serie (Año) [tmdbid-N]/Season 01/Serie S01E01 - 1080p.mkv`, con sus subtítulos;
4. sustituye a las otras versiones de ese episodio que hubiera en la biblioteca.

Si se reinicia Telegarrm a mitad de una descarga, continúa donde estaba. Una descarga no empieza si, al terminar, quedaría menos espacio libre del que indicas en *Ajustes*.

## Seguimiento
El botón «Seguir» de una ficha:
- **En una serie**: cada episodio nuevo se descarga solo, poco después de publicarse.
- **En una película**: se descarga la primera versión que se publique.
- **Mejoras**: si llega una versión mejor de algo que tienes (más resolución, HDR o REMUX), se descarga y sustituye a la anterior. En *Ajustes* puedes elegir conservar las dos.
- **Calidad máxima**: para no descargar, por ejemplo, 4K si no lo quieres.

Solo cuenta lo publicado después de empezar a seguir: para lo anterior están los botones de temporada y de serie completa. Si un archivo está troceado, se espera a que estén todas sus partes.

## Biblioteca
Lo que hay de verdad en tus carpetas de películas y series, con el tamaño, las temporadas y los episodios de cada obra, y un enlace a su ficha.
- **Archivos**: despliega lo que hay dentro, por temporada.
- **Borrar**: una serie, una temporada, una película o un archivo. Siempre pide confirmación y no se puede deshacer. Telegarrm solo puede borrar dentro de las carpetas de la biblioteca.
- **«Revisar la biblioteca»**: si borras o mueves archivos por tu cuenta, fuera de Telegarrm, pulsa este botón. Encuentra lo que has movido y apunta dónde está ahora; lo que has borrado deja de contar como que lo tienes, y podrás volver a descargarlo.

## Actividad
Lo que sigues (con la calidad máxima de cada obra) y un historial de lo que Telegarrm ha hecho solo: episodios nuevos, mejoras, descargas automáticas terminadas o fallidas, archivos borrados.

## Canales
Elige qué canales y grupos de tu cuenta de Telegram vigilar. Al añadir uno, Telegarrm lee todo su historial. Después recibe los mensajes nuevos al momento y, por si acaso, revisa los canales cada 15 minutos. Quitar un canal borra lo que Telegarrm había guardado de él, no los mensajes de Telegram.

## Ajustes
- **Búfer de descargas**: donde se descargan los archivos antes de pasar a la biblioteca. Mejor en el mismo disco que la biblioteca: así el paso es instantáneo.
- **Bibliotecas de películas y de series**.
- **Espacio libre mínimo**.
- **Conservar la versión anterior** al mejorar una descarga.
- **Contraseña de la web**: para cambiarla.

Cada ruta se comprueba al guardarla (que exista, que se pueda escribir y cuánto espacio queda). Cambiar el búfer exige reiniciar Telegarrm, que se hace con el botón «Reiniciar ahora».

## Estado
Si Telegarrm está en marcha, su conexión con Telegram y el inicio de sesión en tu cuenta: teléfono, el código que envía Telegram y, si la tienes, la contraseña de verificación en dos pasos. La sesión se guarda y no hay que repetirlo.

En la cabecera, «Salir» cierra la sesión de la web en ese navegador.
