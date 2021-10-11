:: MIT License
::
:: Copyright (c) 2021 Eric Derewonko
::
:: Permission is hereby granted, free of charge, to any person obtaining a copy
:: of this software and associated documentation files (the "Software"), to deal
:: in the Software without restriction, including without limitation the rights
:: to use, copy, modify, merge, publish, distribute, sublicense, and\or sell
:: copies of the Software, and to permit persons to whom the Software is
:: furnished to do so, subject to the following conditions:
::
:: The above copyright notice and this permission notice shall be included in all
:: copies or substantial portions of the Software.
::
:: THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
:: IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
:: FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
:: AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
:: LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
:: OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
:: SOFTWARE.
::
:: Purpose:
:: This batch will iterate over cached thumbnail files and check if the image number is still in the library.db


@echo OFF
pushd %~dp0

:defaults
:: default values
set cache_thumbs=
set dryrun=1
set mipmap_ext=jpg
set action=echo found stale mipmap: 
call %~dp0\common.cmd
call %~dp0\custom.cmd 2>NUL


:init
:: remember the command line to show it in the end when not purging
set "commandline=%0 %*"

set id_list=%TEMP%\darktable-id_list.%RANDOM%.tmp
set mipmap_list=%TEMP%\darktable-mipmap_list.%RANDOM%.tmp
set mipmap_list_id=%TEMP%\darktable-mipmap_list_id.%RANDOM%.tmp
set mipmap_list_id_2delete=%TEMP%\darktable-mipmap_list_id.%RANDOM%.tmp


:arguments
:: handle command line arguments; warning: there is no check when second needed parameter is missing
for %%o in (%*) DO (
  IF /I "%%~o"=="-h"              call :USAGE && goto :end
  IF /I "%%~o"=="-help"           call :USAGE && goto :end
  IF /I "%%~o"=="--help"          call :USAGE && goto :end
  IF /I "%%~o"=="/?"              call :USAGE && goto :end
      
  IF /I "%%~o"=="-l"              call set "library=%%~2" && shift /1
  IF /I "%%~o"=="--library"       call set "library=%%~2" && shift /1
      
  IF /I "%%~o"=="-c"              call set "cachedir=%%~2" && shift /1
  IF /I "%%~o"=="--cachedir"      call set "cachedir=%%~2" && shift /1
    
  IF /I "%%~o"=="-C"              call set "cache_thumbs=%%~2" && shift /1
  IF /I "%%~o"=="--cache_thumbs"  call set "cache_thumbs=%%~2" && shift /1
    
  IF /I "%%~o"=="-d"              call set "configdir=%%~2" && shift /1
  IF /I "%%~o"=="--configdir"     call set "configdir=%%~2" && shift /1
      
  IF /I "%%~o"=="-p"              set dryrun=0
  IF /I "%%~o"=="--purge"         set dryrun=0

  shift /1
)


:prechecks
:: you cannot concurently access a sqlite database
tasklist /FI "IMAGENAME eq darktable.exe" /FI "STATUS eq running" | findstr darktable.exe && echo error: darktable is running, please exit first && timeout /t 3 && exit /b 1
:: get sqlite3.exe here: https://www.sqlite.org/2017/sqlite-tools-win32-x86-3170000.zip
where sqlite3 >NUL 2>&1 || echo error: sqlite3.exe is not found, please add it somewhere in your PATH first && exit /b 1
:: get sqlite3.exe here: https://frippery.org/files/busybox/busybox.exe
where busybox >NUL 2>&1 || echo error: busybox.exe is not found, please add it somewhere in your PATH first && exit /b 1

:: set the command to run for each stale file
if %dryrun% EQU 0 set action=del /f /q

:: test configdir:
if NOT EXIST "%configdir%" echo error: configdir "%configdir%" doesn't exist && exit /b 1

:: if you force configdir but not library:
if NOT EXIST "%library%" set library=%configdir%\library.db

:: test library:
if NOT EXIST "%library%" echo error: library db "%library%" doesn't exist && exit /b 1

:: the mipmap directory matching the selected library as defined on Linux:
:: echo %LOCALAPPDATA%\darktable\library.db | busybox sha1sum
:: 2917b0700b9fbb133931706610039dffd5f2d65d
:: sha1sum calculation is disabled on Windows until I know how to generate it properly
REM IF NOT DEFINED cache_thumbs (
  REM for /f %%S in ('echo %library% ^| busybox sha1sum') DO set sha1sum=%%S
  REM call set cache_thumbs=%cachedir%\mipmaps-%%sha1sum%%.d
REM )

:: autodetect cache_thumbs instead; there is 0.00001% chance that Windows users know how to handle multiple libraries, let alone cache folders
set cache_thumbs_nb=0
for /f %%c in ('dir /b /AD "%cachedir%\mipmaps-*.d" 2^>NUL') DO (set /A cache_thumbs_nb+=1 && set "cache_thumbs=%cachedir%\%%~c")

:: ensure cache_thumbs is unique
IF %cache_thumbs_nb% GTR 1 echo error: multiple cache_thumbs detected, please call this batch with --cache_thumbs ^<cache_thumbs^> && exit /b 1
:: cache_thumbs not passed as parameter and not found:
IF NOT DEFINED cache_thumbs echo error: cache directory "%cache_thumbs%" not found && exit /b 1
:: cache_thumbs passed as parameter and not found:
IF NOT EXIST "%cache_thumbs%\" echo error: cache directory "%cache_thumbs%" doesn't exist && exit /b 1


::::::::::::::::::::::::::::::::::::::::::::::: main
:main
:: get a list of all image ids from the library
sqlite3 "%library%" "select id from images order by id" >"%id_list%"

:: mipmaps look like this: C:\Users\username\AppData\Local\Microsoft\Windows\INetCache\darktable\mipmaps-2917b0700b9fbb133931706610039dffd5f2d65d.d\0\105.%mipmap_ext%
dir /b /s /A-D "%cache_thumbs%" >"%mipmap_list%"

:: get the uniq id of each mipmap file
busybox sed -r -e "s#.*?\\([0-9]+)\.%mipmap_ext%$#\1#" "%mipmap_list%" | busybox sort -n | uniq >"%mipmap_list_id%"

:: looks for all lines in mipmap_list_id which don't match any line in id_list
:: correct command should be: busybox comm -23 %mipmap_list_id% %id_list% - but for some reason doesn't work on Windows
busybox grep -Fxv -f %id_list% %mipmap_list_id% >"%mipmap_list_id_2delete%"

:: shortcut to exit
for %%f in ("%mipmap_list_id_2delete%") do set mipmap_list_id_2delete_size=%%~zf
IF %mipmap_list_id_2delete_size% EQU 0 echo no leftover thumbnails for deleted images found && goto :end

:: finally, delete all mipmaps missing from the database, under each of the 0-8 level folders:
for /L %%N in (0,1,8) DO for /f %%i in (%mipmap_list_id_2delete%) DO %action% "%cache_thumbs%\%%N\%%i.%mipmap_ext%" 2>NUL

if %dryrun% EQU 1 (
  echo:
  echo to really remove stale thumbnails from the cache, call:
  echo %commandline% --purge
)

goto :end
::::::::::::::::::::::::::::::::::::::::::::::: main


:USAGE
echo Delete thumbnails of images that are no longer in darktable's library
echo Usage:   %~n0 [options]
echo:
echo Options:
echo   -c^|--cachedir ^<path^>   path to the place where darktable's cache folder
echo                            (default: "%cachedir%")
echo   -C^|--cache_thumbs ^<path^>   path to the place where darktable's thumbnail caches are stored
echo                            (default: auto-detected "%cachedir%\mipmaps-sha1sum.d")
echo   -d^|--configdir ^<path^>    path to the darktable config directory
echo                            (default: "%configdir%")
echo   -l^|--library ^<path^>      path to the library.db
echo                            (default: "%library%")
echo   -p^|--purge               actually delete the files instead of just finding them
goto :EOF

:end
:: cleanup
del /f /q "%id_list%" "%mipmap_list%" "%mipmap_list_id%" "%mipmap_list_id_2delete%"
