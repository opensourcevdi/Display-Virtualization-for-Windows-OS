set "INF_FILE=x64\Debug\DVServer\DVServer.inf"

pushd "%~dp0"
:start

@powershell -NoProfile -Command ^
  "$d = Get-Date;" ^
  "$driverVer = 'DriverVer = ' + $d.ToString('MM/dd/yyyy') + ',' + $d.ToString('yyMM.dd.HH.mm');" ^
  "Write-Host ('New DriverVer line:');" ^
  "Write-Host $driverVer;" ^
  "$content = Get-Content '%INF_FILE%';" ^
  "$content = $content -replace '^DriverVer\s*=.*$', $driverVer;" ^
  "$content | Set-Content '%INF_FILE%';" ^
  "Write-Host ('Updated %INF_FILE%.'); "

@set "devcon=C:\Program Files (x86)\Windows Kits\*\Tools\*\x64\devcon.exe"
@for /f "delims=" %%v in (
  'powershell -NoProfile -Command "(Resolve-Path $ENV:devcon).Path"'
)  do @set "devcon=%%v"
"%devcon%" update "%INF_FILE%" "SWC\DVServer"

@REM pnputil /add-driver "%INF_FILE%" /install /force
@REM @for /f "delims=" %%v in ('powershell -NoProfile -Command ^
@REM  "$device = Get-PnpDevice -PresentOnly" ^
@REM  "| Where-Object { $_.HardwareID -contains 'SWC\DVServer' };" ^
@REM  "$device.InstanceId;"') do set "device_id=%%v"
@REM pnputil /restart-device "%device_id%"

:done
pause

goto start

:end
popd
