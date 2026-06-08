$bat = 'D:\S4W 1.1\Hook\do_compile.bat'
$out = 'D:\S4W 1.1\Hook\compile_out.txt'
$result = & cmd.exe /c "`"$bat`" > `"$out`" 2>&1"
Get-Content $out
