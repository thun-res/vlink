function Controller()
{
    if (installer.isUninstaller()) {
        installer.setDefaultPageVisible(QInstaller.TargetDirectory,    false);
        installer.setDefaultPageVisible(QInstaller.ComponentSelection, false);
        installer.setDefaultPageVisible(QInstaller.LicenseCheck,       false);
        installer.setDefaultPageVisible(QInstaller.StartMenuSelection, false);
    }
}
