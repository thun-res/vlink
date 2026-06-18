function Component()
{
}

Component.prototype.beginInstallation = function()
{
    component.beginInstallation();

    var targetDir = String(installer.value("TargetDir") || "");
    var homeDir   = String(installer.value("HomeDir")   || "");

    var norm = function(p) {
        return p.replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase();
    };
    var nt = norm(targetDir);
    var nh = norm(homeDir);

    var deniedExact = [
        "",
        nh,
        "/", "/root", "/home", "/usr", "/usr/local", "/usr/share",
        "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/lib64", "/usr/include",
        "/etc", "/opt", "/var", "/bin", "/sbin", "/lib", "/lib64",
        "/boot", "/dev", "/proc", "/sys", "/tmp", "/run",
        "/srv", "/mnt", "/media",
        "/applications", "/library", "/system", "/users", "/volumes",
        nh + "/desktop",   nh + "/documents", nh + "/downloads",
        nh + "/pictures",  nh + "/music",     nh + "/videos",
        nh + "/templates", nh + "/public",
        nh + "/.config",   nh + "/.local",    nh + "/.cache",
        nh + "/.ssh",      nh + "/.gnupg",
        nh + "/library",   nh + "/appdata",
        nh + "/appdata/local", nh + "/appdata/roaming",
        nh + "/onedrive"
    ];

    var deniedRegex = [
        /^[a-z]:$/,
        /^[a-z]:\/windows$/,
        /^[a-z]:\/program files( \(x86\))?$/,
        /^[a-z]:\/programdata$/,
        /^[a-z]:\/users$/,
        /^[a-z]:\/users\/public$/,
        /^[a-z]:\/system volume information$/,
        /^[a-z]:\/recovery$/,
        /^[a-z]:\/\$recycle\.bin$/
    ];

    var blocked = false;
    for (var i = 0; i < deniedExact.length; ++i) {
        if (nt === deniedExact[i]) { blocked = true; break; }
    }
    if (!blocked) {
        for (var j = 0; j < deniedRegex.length; ++j) {
            if (deniedRegex[j].test(nt)) { blocked = true; break; }
        }
    }

    if (blocked) {
        throw new Error("Refusing to install to '" + targetDir + "': this is a system, shared, or user-data directory. Uninstalling would remove the entire folder and any files it contains. Please choose a dedicated subdirectory such as '" + homeDir + "/vlink'.");
    }
};

Component.prototype.createOperations = function()
{
    component.createOperations();

    var targetDir = installer.value("TargetDir");

    if (systemInfo.kernelType === "winnt") {
        var targetWin = targetDir.replace(/\//g, "\\");

        component.addOperation("Execute",
            "cmd.exe", "/c", targetWin + "\\install.bat",
            "UNDOEXECUTE",
            "cmd.exe", "/c", targetWin + "\\uninstall.bat");
    } else {
        var safe = targetDir.replace(/'/g, "'\\''");
        component.addOperation("Execute",
            "bash", "-c",
            "cd '" + safe + "' && { chmod +x install.sh uninstall.sh bin/* >/dev/null 2>&1; true; } && bash install.sh",
            "UNDOEXECUTE",
            "bash", "-c",
            "cd '" + safe + "' && bash uninstall.sh");
    }
};
