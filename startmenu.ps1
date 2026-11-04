# Start menu entries for the AT6301 panel (Windows): "AT6301" and
# "AT6301 (simulated)". "make install" and "make uninstall" run this:
#
#     startmenu.ps1 install <folder holding at6301.exe>
#     startmenu.ps1 uninstall
#
param([string]$action, [string]$dir)

$programs = [Environment]::GetFolderPath('Programs')
$entries = @(
    @{ name = 'AT6301';             args = '';      text = 'AT6301 front panel' },
    @{ name = 'AT6301 (simulated)'; args = '--sim'; text = 'AT6301 front panel with a simulated supply' }
)

foreach ($e in $entries) {
    $path = Join-Path $programs ($e.name + '.lnk')
    if ($action -eq 'install') {
        $link = (New-Object -ComObject WScript.Shell).CreateShortcut($path)
        $link.TargetPath = Join-Path $dir 'at6301.exe'
        $link.Arguments = $e.args
        $link.WorkingDirectory = $dir
        $link.Description = $e.text
        $link.Save()
        "Start menu: $($e.name)"
    } elseif ($action -eq 'uninstall') {
        Remove-Item -ErrorAction SilentlyContinue $path
    } else {
        Write-Error "startmenu.ps1: install <folder> or uninstall"
        exit 1
    }
}
