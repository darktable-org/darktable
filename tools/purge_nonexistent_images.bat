::
:: Usage: purge_nonexistent_images [-p]
::        -p  do the purge, otherwise only display nonexistent images
::

@echo off
where.exe /q sqlite3
if %ERRORLEVEL% EQU 1 (
  echo Error: sqlite3.exe is not installed or is not in PATH
  exit 1
)

tasklist.exe | find.exe /I "darktable.exe"
IF %ERRORLEVEL% EQU 0 (
  echo Error: darktable is running, please exit first
  exit 1
)


set DBFILE=%LOCALAPPDATA%\darktable\library.db
set dryrun=1
set library=
set configdir=

:: Remember the script file name for the usage text
set scriptfilename=%~nx0

:: Remember the command line to show it in the end when not purging
if "%*" == "" (
  set commandline=%~nx0
) else (
  set commandline=%~nx0 %*
)

:: Handle command line arguments
:GETOPTS
if /I "%1" == "-h" goto HELP
if /I "%1" == "--help" goto HELP
if /I "%1" == "-l" set "library=%2"& shift
if /I "%1" == "--library" set library="%2" & shift
if /I "%1" == "-c" set "configdir=%2"& shift
if /I "%1" == "--configdir" set "configdir=%2"& shift
if /I "%1" == "-p" set dryrun=0
if /I "%1" == "--purge" set dryrun=0
shift
if not "%1" == "" goto GETOPTS

:: When user sets configdir, it means we should work with library.db in that directory
if not .%configdir%==. (
  set DBFILE=%configdir%\library.db
)

if not .%library%==. (
  set DBFILE=%library%
)

if not exist %DBFILE% (
  echo Error: library file '%DBFILE%' doesn't exist
  exit 1
) else (
  echo The library we are working with is '%DBFILE%'
  echo:
)

set QUERY="SELECT images.id, film_rolls.folder || '\' || images.filename FROM images JOIN film_rolls ON images.film_id = film_rolls.id"

if %dryrun% EQU 0 (
  echo Removing the following nonexistent files:
) else (
  echo The following nonexistent files will be deleted during a non-dry run:
)

set ids=

setlocal EnableDelayedExpansion
for /f "tokens=1* delims=|" %%a in ('sqlite3.exe %DBFILE% %QUERY%') do (
  if not exist %%b (
    echo.  %%b with ID %%a
    if .!ids!==. (
      set ids=%%a
    ) else (
      set ids=!ids!,%%a
    )
  )
)

if %dryrun% EQU 0 (
  echo This is NOT dry run
  for %%a in (images,meta_data) do (
      echo Removing from %%a...
      sqlite3 %DBFILE% "DELETE FROM %%a WHERE id IN (%ids%)"
  )

  for %%a in (color_labels,history,masks_history,selected_images,tagged_images,history_hash,module_order) do (
      echo Removing from %%a...
      sqlite3 %DBFILE% "DELETE FROM %%a WHERE imgid in (%ids%)"
  )

  rem delete now-empty film rolls
  sqlite3 %DBFILE% "DELETE FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"
  sqlite3 %DBFILE% "VACUUM; ANALYZE"
) else (
  echo:
  echo The following now-empty filmrolls will be removed during a non-dry run:
  sqlite3 %DBFILE% "SELECT folder FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"

  echo:
  echo To really remove nonexistent images from the database, run:
  echo %commandline% --purge
)

exit 0

:HELP
echo Remove nonexistent images from darktable's database
echo Usage:   %scriptfilename% [options]
echo:
echo Options:
echo:   -c^|--configdir ^<path^>    path to the darktable config directory in which the library.db file will be used
echo:                            (default: '%configdir%')
echo:   -l^|--library ^<path^>      path to the library database file
echo:                            (default: '%DBFILE%')
echo:   -p^|--purge               actually purge the nonexistent images instead of just finding them
exit 0
