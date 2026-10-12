Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$f = New-Object System.Windows.Forms.Form
$f.FormBorderStyle = 'None'
$f.BackColor = [System.Drawing.Color]::FromArgb(0, 255, 0)
$f.StartPosition = 'Manual'
$f.Location = New-Object System.Drawing.Point(300, 300)
$f.Size = New-Object System.Drawing.Size(400, 300)
$f.TopMost = $true
$f.Show()
Start-Sleep -Seconds 14
$f.Close()
