@echo off
rem Nintendo Land Recomp - Determinismus-Test fuer Netplay
rem Startet das Spiel zweimal ohne Eingaben im deterministischen Modus und speichert beide Logs.
cd /d "%~dp0"
set RPX=C:\Users\offic\Documents\cemu wiiu gaymes\Nintendo Land [Game] [0005000010102100]\code\Lunch.rpx
set NL_DET=1
set NL_DET_HASH=1
set NL_DET_HASH_EVERY=300
set NL_DET_QUIT_AFTER=3600
set NL_DET_TRACE=1
taskkill /f /im NintendoLand.exe >nul 2>&1

echo Sichere Spielstand, damit beide Laeufe gleich starten ...
robocopy "nl_data\mlc01" "nl_data\_det_backup" /MIR /NFL /NDL /NJH /NJS >nul

echo.
echo Lauf 1 von 2 laeuft (ca. 1 Minute). Bitte NICHTS druecken, Maus weg vom Fenster.
start /wait "" NintendoLand.exe "%RPX%"
copy /y "nl_data\log.txt" "nl_data\det_lauf1.txt" >nul

echo Stelle Spielstand wieder her ...
robocopy "nl_data\_det_backup" "nl_data\mlc01" /MIR /NFL /NDL /NJH /NJS >nul

echo.
echo Lauf 2 von 2 laeuft (ca. 1 Minute). Bitte wieder NICHTS druecken.
start /wait "" NintendoLand.exe "%RPX%"
copy /y "nl_data\log.txt" "nl_data\det_lauf2.txt" >nul

robocopy "nl_data\_det_backup" "nl_data\mlc01" /MIR /NFL /NDL /NJH /NJS >nul
echo.
echo Fertig! Sag Claude Bescheid, er vergleicht die beiden Laeufe.
pause
