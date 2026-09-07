#!/usr/bin/env python3.12
import argparse
import datetime as dt
import json
import re
import shutil
import shlex
import subprocess
import sys
import os
import urllib.request
from pathlib import Path

scriptVersion = "v2.10 (2026-09-06)"
defaultAwsServer = "admin@aandewiel.nl"
defaultAwsTarget = "/home/admin/flasherWebsite_v3"
defaultAwsSshKey = "~/.ssh/LightsailDefaultKey-eu-central-1.pem"
defaultProjectImageUrl = "https://flasher.aandewiel.nl/projects/ESP32project.png"

versionPattern = re.compile(r"v\d+\.\d+\.\d+")
envSectionPattern = re.compile(r"^\s*\[\s*env:([^\]]+)\s*\]\s*$")
semverPattern = re.compile(r"(\d+)\.(\d+)\.(\d+)")
versionWithPrefixPattern = re.compile(r"[vV](\d+\.\d+\.\d+)")
workspaceDirPattern = re.compile(r"^\s*workspace_dir\s*=\s*(.+?)\s*$", re.IGNORECASE)
fsStartPattern = re.compile(r"_FS_start\s*=\s*(0x[0-9a-fA-F]+|\d+)")
envNotePattern = re.compile(r"^\s*[;#]\s*note\s*:\s*(.*?)\s*$", re.IGNORECASE)
commentLinePattern = re.compile(r"^\s*[;#](.*)$")
partitionTableFilenamePattern = re.compile(r'^CONFIG_PARTITION_TABLE_FILENAME="([^"]+)"', re.MULTILINE)


def parsePlatformioSections(platformioIni: Path) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    currentSection = None

    for rawLine in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue

        sectionMatch = re.match(r"^\[(.+)\]$", line)
        if sectionMatch:
            currentSection = sectionMatch.group(1).strip().lower()
            if currentSection not in sections:
                sections[currentSection] = {}
            continue

        if currentSection is None or "=" not in rawLine:
            continue

        key, value = rawLine.split("=", 1)
        normalizedKey = key.strip().lower()
        normalizedValue = re.split(r"\s[;#]", value, maxsplit=1)[0].strip()
        sections[currentSection][normalizedKey] = normalizedValue

    return sections


def getEnvConfigValue(
    sections: dict[str, dict[str, str]], envName: str, key: str) -> str | None:
    normalizedKey = key.strip().lower()
    envSection = f"env:{envName}".lower()

    if envSection in sections and normalizedKey in sections[envSection]:
        return sections[envSection][normalizedKey]

    if "env" in sections and normalizedKey in sections["env"]:
        return sections["env"][normalizedKey]

    return None


def sanitizePathSegment(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "_", value.strip())
    sanitized = sanitized.strip("._-")
    if not sanitized:
        return "unknown"
    return sanitized


def resolveEnvBoardName(sections: dict[str, dict[str, str]], envName: str) -> str:
    configuredBoard = getEnvConfigValue(sections, envName, "board")
    if configuredBoard:
        return sanitizePathSegment(configuredBoard)
    return sanitizePathSegment(envName)


def resolveEnvPlatformName(
    sections: dict[str, dict[str, str]], envName: str
) -> str | None:
    configuredPlatform = getEnvConfigValue(sections, envName, "platform")
    if configuredPlatform:
        return configuredPlatform.strip().lower()
    return None


def detectSocFamily(boardName: str, platformName: str | None) -> str:
    normalizedBoard = re.sub(r"[^a-z0-9]", "", (boardName or "").lower())
    normalizedPlatform = re.sub(r"[^a-z0-9]", "", (platformName or "").lower())

    esp8266BoardAliases = {
        "d1mini",
        "d1mini32",
        "d1minipro",
        "nodemcu",
        "nodemcuv2",
        "esp01",
        "esp01s",
        "esp12e",
        "esp12f",
        "esp07",
        "esp07s",
        "esp8285",
    }

    if (
        "esp8266" in normalizedBoard
        or "esp8266" in normalizedPlatform
        or "8266" in normalizedBoard
        or "8266" in normalizedPlatform
        or normalizedBoard in esp8266BoardAliases
    ):
        return "esp8266"

    return "esp32"


def resolveEnvPartitionsSource(
    projectRoot: Path,
    sections: dict[str, dict[str, str]],
    envName: str,
    socFamily: str,
) -> Path | None:
    configuredValue = None
    envSection = f"env:{envName}".lower()

    if socFamily == "esp8266":
        envValues = sections.get(envSection, {})
        configuredValue = envValues.get("board_build.partitions")
    else:
        configuredValue = getEnvConfigValue(sections, envName, "board_build.partitions")

    candidates: list[Path] = []
    if configuredValue:
        cleaned = configuredValue.strip().strip('"').strip("'")
        cleaned = cleaned.replace("${PROJECT_DIR}", str(projectRoot))
        cleaned = cleaned.replace("$PROJECT_DIR", str(projectRoot))
        resolved = Path(cleaned).expanduser()
        if not resolved.is_absolute():
            resolved = (projectRoot / resolved).resolve()
        else:
            resolved = resolved.resolve()
        candidates.append(resolved)

    if socFamily == "esp32":
        defaultCandidate = (projectRoot / "partitions.csv").resolve()
        candidates.append(defaultCandidate)

    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return candidate

    return None


def resolveEnvLdscriptSource(
    projectRoot: Path,
    sections: dict[str, dict[str, str]],
    envName: str,
    socFamily: str,
) -> Path | None:
    if socFamily != "esp8266":
        return None

    configuredValue = getEnvConfigValue(sections, envName, "board_build.ldscript")
    if not configuredValue:
        return None

    cleaned = configuredValue.strip().strip('"').strip("'")
    cleaned = cleaned.replace("${PROJECT_DIR}", str(projectRoot))
    cleaned = cleaned.replace("$PROJECT_DIR", str(projectRoot))
    resolved = Path(cleaned).expanduser()
    if not resolved.is_absolute():
        resolved = (projectRoot / resolved).resolve()
    else:
        resolved = resolved.resolve()

    if resolved.exists() and resolved.is_file():
        return resolved

    return None


def resolveGeneratedEsp8266Ldscript(buildDir: Path) -> Path | None:
    ldDir = buildDir / "ld"
    if not ldDir.exists() or not ldDir.is_dir():
        return None

    preferredNames = [
        "local.eagle.app.v6.common.ld",
        "eagle.app.v6.common.ld",
    ]

    for name in preferredNames:
        candidate = ldDir / name
        if candidate.exists() and candidate.is_file():
            return candidate

    for child in sorted(ldDir.iterdir()):
        if child.is_file() and child.suffix == ".ld":
            return child

    return None


def parsePartitionsCsv(partitionsCsvPath: Path) -> dict[str, dict[str, str]]:
    partitions: dict[str, dict[str, str]] = {}

    for rawLine in partitionsCsvPath.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith("#"):
            continue

        parts = [part.strip() for part in rawLine.split(",")]
        if len(parts) < 4:
            continue

        name = parts[0]
        if not name:
            continue

        partitions[name] = {
            "name": name,
            "type": parts[1] if len(parts) > 1 else "",
            "subtype": parts[2] if len(parts) > 2 else "",
            "offset": parts[3] if len(parts) > 3 else "",
            "size": parts[4] if len(parts) > 4 else "",
        }

    return partitions


def detectFirmwareOffset(partitions: dict[str, dict[str, str]], socFamily: str) -> str:
    if socFamily == "esp8266":
        firmwareEntry = partitions.get("firmware") if isinstance(partitions, dict) else None
        if firmwareEntry and firmwareEntry.get("offset"):
            return firmwareEntry["offset"]
        return "0x00000"

    if "factory" in partitions and partitions["factory"].get("offset"):
        return partitions["factory"]["offset"]
    if "app0" in partitions and partitions["app0"].get("offset"):
        return partitions["app0"]["offset"]
    if "ota_0" in partitions and partitions["ota_0"].get("offset"):
        return partitions["ota_0"]["offset"]
    if "firmware" in partitions and partitions["firmware"].get("offset"):
        return partitions["firmware"]["offset"]

    for part in partitions.values():
        partType = str(part.get("type") or "").strip().lower()
        partSubtype = str(part.get("subtype") or "").strip().lower()

        if partType in {"app", "0", "0x00"} and part.get("offset"):
            return part["offset"]

        if partSubtype in {"factory", "app0", "ota_0", "ota0"} and part.get("offset"):
            return part["offset"]

    if socFamily == "esp8266":
        return "0x00000"

    return "0x10000"


def detectFilesystemOffset(partitions: dict[str, dict[str, str]]) -> str | None:
    directNames = ["spiffs", "littlefs", "fatfs", "storage"]
    for name in directNames:
        if name in partitions and partitions[name].get("offset"):
            return partitions[name]["offset"]

    for part in partitions.values():
        subtype = (part.get("subtype") or "").lower()
        if subtype in {"spiffs", "littlefs", "fatfs"} and part.get("offset"):
            return part["offset"]

    return None


def detectEsp8266FilesystemOffsetFromLdscript(ldscriptPath: Path) -> str | None:
    if not ldscriptPath.exists() or not ldscriptPath.is_file():
        return None

    content = ldscriptPath.read_text(encoding="utf-8", errors="ignore")
    match = fsStartPattern.search(content)
    if not match:
        return None

    rawValue = match.group(1)
    try:
        numericValue = int(rawValue, 0)
    except ValueError:
        return None

    if numericValue >= 0x40200000:
        numericValue = numericValue - 0x40200000

    if numericValue < 0:
        return None

    return f"0x{numericValue:05X}"


def isEsp32S3Board(boardName: str) -> bool:
    normalized = re.sub(r"[^a-z0-9]", "", boardName.lower())
    return "esp32s3" in normalized


def generateFlashJson(
    targetVersionDir: Path,
    boardName: str,
    version: str,
    socFamily: str,
    ldscriptSource: Path | None,
    logLines: list[str],
) -> None:
    partitionsCsvPath = targetVersionDir / "partitions.csv"
    partitions: dict[str, dict[str, str]] = {}

    if partitionsCsvPath.exists():
        try:
            partitions = parsePartitionsCsv(partitionsCsvPath)
        except Exception as exc:
            logLines.append(f"WARN: partitions.csv parse failed: {exc}")

    flashFiles: list[dict[str, str]] = []
    if socFamily == "esp32":
        bootloaderOffset = "0x0000" if isEsp32S3Board(boardName) else "0x1000"

        bootloaderPath = targetVersionDir / "bootloader.bin"
        if bootloaderPath.exists():
            flashFiles.append({"offset": bootloaderOffset, "file": "bootloader.bin"})

        partitionsBinPath = targetVersionDir / "partitions.bin"
        if partitionsBinPath.exists():
            flashFiles.append({"offset": "0x8000", "file": "partitions.bin"})

        bootAppPath = targetVersionDir / "boot_app0.bin"
        if bootAppPath.exists():
            flashFiles.append({"offset": "0xe000", "file": "boot_app0.bin"})

    firmwarePath = targetVersionDir / "firmware.bin"
    if firmwarePath.exists():
        firmwareOffset = detectFirmwareOffset(partitions, socFamily)
        flashFiles.append({"offset": firmwareOffset, "file": "firmware.bin"})

    filesystemFile = None
    if (targetVersionDir / "storage.bin").exists():
        filesystemFile = "storage.bin"

    if filesystemFile:
        filesystemOffset = detectFilesystemOffset(partitions)
        if not filesystemOffset and socFamily == "esp8266" and ldscriptSource:
            filesystemOffset = detectEsp8266FilesystemOffsetFromLdscript(ldscriptSource)
            if filesystemOffset:
                logLines.append(f"Derived filesystem offset from ldscript: {filesystemOffset}")

        if not filesystemOffset and socFamily == "esp8266":
            filesystemOffset = "0x300000"
            logLines.append(
                "WARN: Falling back to default ESP8266 filesystem offset 0x300000"
            )

        if filesystemOffset:
            flashFiles.append({"offset": filesystemOffset, "file": filesystemFile})
        else:
            logLines.append(
                f"WARN: No filesystem offset found in partitions.csv for {filesystemFile}"
            )

    flashPayload = {
        "board": boardName,
        "soc": socFamily,
        "version": version,
        "flash_files": flashFiles,
    }
    (targetVersionDir / "flash.json").write_text(
        json.dumps(flashPayload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def parseEnvs(platformioIni: Path) -> list[str]:
    envs: list[str] = []
    for line in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = envSectionPattern.match(line)
        if match:
            envs.append(match.group(1).strip())

    seen = set()
    uniqueEnvs = []
    for env in envs:
        if env and env not in seen:
            uniqueEnvs.append(env)
            seen.add(env)

    return uniqueEnvs


def parseEnvNotes(platformioIni: Path) -> dict[str, str]:
    envNotes: dict[str, str] = {}
    pendingNextEnvNote = None
    currentEnv = None
    lines = platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines()
    index = 0

    while index < len(lines):
        rawLine = lines[index]
        line = rawLine.strip()

        envMatch = envSectionPattern.match(line)
        if envMatch:
            currentEnv = envMatch.group(1).strip()
            if pendingNextEnvNote and currentEnv and currentEnv not in envNotes:
                envNotes[currentEnv] = pendingNextEnvNote
            pendingNextEnvNote = None
            index += 1
            continue

        genericSectionMatch = re.match(r"^\s*\[(.+)\]\s*$", line)
        if genericSectionMatch:
            currentEnv = None
            index += 1
            continue

        noteMatch = envNotePattern.match(rawLine)
        if noteMatch:
            collectedChunks: list[str] = []
            firstChunk = noteMatch.group(1).strip()
            if firstChunk:
                collectedChunks.append(firstChunk)

            probeIndex = index + 1
            while probeIndex < len(lines):
                probeLineRaw = lines[probeIndex]
                probeLine = probeLineRaw.strip()

                if re.match(r"^\s*\[.+\]\s*$", probeLine):
                    break

                if not probeLine:
                    probeIndex += 1
                    continue

                probeCommentMatch = commentLinePattern.match(probeLineRaw)
                if not probeCommentMatch:
                    break

                probeChunk = probeCommentMatch.group(1).strip()
                if probeChunk and not re.match(r"^note\s*:", probeChunk, re.IGNORECASE):
                    collectedChunks.append(probeChunk)
                probeIndex += 1

            mergedNote = " ".join([chunk for chunk in collectedChunks if chunk]).strip()
            if mergedNote:
                if currentEnv:
                    envNotes[currentEnv] = mergedNote
                else:
                    pendingNextEnvNote = mergedNote

            index = probeIndex
            continue

        index += 1

    return envNotes


def getWorkspaceDir(platformioIni: Path, projectPath: Path) -> Path:
    sectionName = ""
    workspaceValue = None

    for rawLine in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue

        sectionMatch = re.match(r"^\[(.+)\]$", line)
        if sectionMatch:
            sectionName = sectionMatch.group(1).strip().lower()
            continue

        if sectionName != "platformio":
            continue

        keyMatch = workspaceDirPattern.match(rawLine)
        if keyMatch:
            workspaceValue = keyMatch.group(1).strip()
            break

    if not workspaceValue:
        return projectPath / ".pio"

    expanded = workspaceValue
    expanded = expanded.replace("${PROJECT_DIR}", str(projectPath))
    expanded = expanded.replace("$PROJECT_DIR", str(projectPath))
    expanded = expanded.replace("${platformio.packages_dir}", "")
    resolved = Path(expanded).expanduser()
    if not resolved.is_absolute():
        resolved = (projectPath / resolved).resolve()
    else:
        resolved = resolved.resolve()
    return resolved


def normalizeVersion(versionValue: str) -> str:
    match = semverPattern.search(versionValue)
    if not match:
        return "v0.0.0"
    return f"v{match.group(1)}.{match.group(2)}.{match.group(3)}"


def detectVersion(projectPath: Path) -> str:
    #-- PlatformIO sources live under src/, ESP-IDF sources live under main/
    for srcDir in (projectPath / "src", projectPath / "main"):
        if not srcDir.is_dir():
            continue

        for filePath in sorted(srcDir.rglob("*")):
            if not filePath.is_file():
                continue

            text = filePath.read_text(encoding="utf-8", errors="ignore")
            if "PROG_VERSION" not in text:
                continue

            for line in text.splitlines():
                if "PROG_VERSION" not in line:
                    continue

                prefixedMatch = versionWithPrefixPattern.search(line)
                if prefixedMatch:
                    return f"v{prefixedMatch.group(1)}"

                semverMatch = semverPattern.search(line)
                if semverMatch:
                    return f"v{semverMatch.group(1)}.{semverMatch.group(2)}.{semverMatch.group(3)}"

                fallbackMatch = versionPattern.search(line)
                if fallbackMatch:
                    return normalizeVersion(fallbackMatch.group(0))

    return "v0.0.0"


def runCommand(cmd: list[str], cwd: Path, logLines: list[str]) -> None:
    logLines.append(f"$ {' '.join(cmd)}")
    process = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)
    if process.stdout:
        logLines.append(process.stdout.rstrip())
    if process.stderr:
        logLines.append(process.stderr.rstrip())
    if process.returncode != 0:
        raise RuntimeError(
            f"Command failed ({process.returncode}): {' '.join(cmd)}\n{process.stderr}"
        )


def discoverBuildDir(projectRoot: Path, workspaceDir: Path, envName: str) -> Path:
    directCandidate = workspaceDir / "build" / envName
    if directCandidate.exists():
        return directCandidate

    buildRoot = workspaceDir / "build"
    if buildRoot.exists():
        for child in sorted(buildRoot.iterdir()):
            if not child.is_dir():
                continue
            if child.name == envName:
                return child
            if envName in child.name and (child / "firmware.bin").exists():
                return child

    fallbackPio = projectRoot / ".pio" / "build" / envName
    if fallbackPio.exists():
        return fallbackPio

    raise RuntimeError(
        f"Build directory not found for env '{envName}' in workspace_dir '{workspaceDir}' or fallback '.pio'."
    )


def ensureProjectMetaDataDefaults(rootDir: Path) -> Path:
    metaDataDir = rootDir / "projectMetaData"
    if metaDataDir.exists() and metaDataDir.is_dir():
        return metaDataDir

    metaDataDir.mkdir(parents=True, exist_ok=True)

    (metaDataDir / "project_en.md").write_text(
        "# your_project_name\n\nDiscription in English\n", encoding="utf-8"
    )
    (metaDataDir / "project_nl.md").write_text(
        "# your_project_name\n\nBeschrijving van het project in Dutch\n", encoding="utf-8"
    )

    payload = {
        "name": "your_project_name",
        "long_name_nl": "Langere naam in Dutch",
        "long_name_en": "longer name in English",
        "description_en": "Discription in English",
        "description_nl": "Beschrijving van het project in Dutch",
        "github_url": "https://github.com/mrWheel/",
        "post_url": "https://willem.aandewiel.nl/",
        "image": "thisProject.png",
    }
    (metaDataDir / "project.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    targetImage = metaDataDir / "thisProject.png"
    try:
        urllib.request.urlretrieve(defaultProjectImageUrl, str(targetImage))
    except Exception:
        targetImage.touch()

    return metaDataDir


def copyProjectMetaData(metaDataDir: Path, targetProjectDir: Path) -> None:
    for item in sorted(metaDataDir.iterdir()):
        if not item.is_file():
            continue

        destinationName = item.name
        if item.name == "ESP32project.png":
            destinationName = "thisProject.png"

        shutil.copy2(item, targetProjectDir / destinationName)


def copyIfExists(source: Path, destination: Path) -> bool:
    if source.exists() and source.is_file():
        shutil.copy2(source, destination)
        return True
    return False


def collectAndCopyArtifacts(
    projectRoot: Path,
    workspaceDir: Path,
    envName: str,
    boardName: str,
    socFamily: str,
    targetVersionDir: Path,
    envPartitionsSource: Path | None,
    envLdscriptSource: Path | None,
    version: str,
    logLines: list[str],
) -> None:
    buildDir = discoverBuildDir(projectRoot, workspaceDir, envName)
    effectiveLdscriptSource = envLdscriptSource

    if socFamily == "esp8266" and not effectiveLdscriptSource:
        effectiveLdscriptSource = resolveGeneratedEsp8266Ldscript(buildDir)
        if effectiveLdscriptSource:
            logLines.append(f"Using generated ldscript source: {effectiveLdscriptSource}")

    required = buildDir / "firmware.bin"
    if not required.exists():
        raise RuntimeError(f"firmware.bin not found for env '{envName}'")

    shutil.copy2(required, targetVersionDir / "firmware.bin")

    optionalFiles = [
        "boot_app0.bin",
        "bootloader.bin",
        "partitions.bin",
        "partitions.csv",
    ]
    for name in optionalFiles:
        copyIfExists(buildDir / name, targetVersionDir / name)

    targetPartitionsCsv = targetVersionDir / "partitions.csv"
    if envPartitionsSource and envPartitionsSource.exists():
        shutil.copy2(envPartitionsSource, targetPartitionsCsv)
        logLines.append(f"Using partitions source: {envPartitionsSource}")

    if effectiveLdscriptSource and effectiveLdscriptSource.exists():
        shutil.copy2(effectiveLdscriptSource, targetVersionDir / "ldscript.ld")
        if envLdscriptSource and envLdscriptSource.exists():
            logLines.append(f"Using ldscript source: {envLdscriptSource}")

    fsCandidates = [
        ("spiffs.bin", "storage.bin"),
        ("littlefs.bin", "storage.bin"),
        ("LittleFS.bin", "storage.bin"),
    ]
    for sourceName, destName in fsCandidates:
        if copyIfExists(buildDir / sourceName, targetVersionDir / destName):
            break

    generateFlashJson(
        targetVersionDir,
        boardName,
        version,
        socFamily,
        effectiveLdscriptSource,
        logLines,
    )

    buildLogPath = targetVersionDir / "build_log.md"
    now = dt.datetime.now().isoformat(timespec="seconds")
    logBody = [f"# Build log for {envName}", "", f"Generated: {now}", ""]
    logBody.extend(logLines)
    logBody.append("")
    logBody.append(f"Resolved buildDir: {buildDir}")
    buildLogPath.write_text("\n".join(logBody).strip() + "\n", encoding="utf-8")


def resolveExecutable(commandName: str, preferredPaths: list[str]) -> str:
    for preferredPath in preferredPaths:
        pathObj = Path(preferredPath)
        if pathObj.exists() and os.access(pathObj, os.X_OK):
            return str(pathObj)

    detectedPath = shutil.which(commandName)
    if detectedPath:
        return detectedPath

    raise RuntimeError(f"Executable not found: {commandName}")


def syncProjectToAws(
    projectsRoot: Path,
    projectName: str,
    awsServer: str,
    awsTarget: str,
    awsSshKey: Path,
    awsDryRun: bool,
) -> None:
    rsyncPath = resolveExecutable("rsync", ["/usr/bin/rsync"])
    sshPath = resolveExecutable("ssh", ["/usr/bin/ssh"])

    sourceProjectPath = projectsRoot / projectName
    if not sourceProjectPath.exists():
        raise RuntimeError(f"Project directory does not exist for sync: {sourceProjectPath}")

    remoteProjectBase = f"{awsTarget.rstrip('/')}/projects"
    remoteProjectPath = f"{remoteProjectBase}/{projectName}"

    mkdirCmd = [
        sshPath,
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        "-i",
        str(awsSshKey),
        awsServer,
        f"mkdir -p {shlex.quote(remoteProjectPath)}",
    ]
    mkdirProcess = subprocess.run(mkdirCmd, text=True, capture_output=True)
    if mkdirProcess.returncode != 0:
        raise RuntimeError(
            f"Failed to create AWS project directory: {mkdirProcess.stderr.strip() or mkdirProcess.stdout.strip()}"
        )

    sshRsyncTransport = (
        f"{sshPath} -o BatchMode=yes -o ConnectTimeout=10 -i {shlex.quote(str(awsSshKey))}"
    )

    rsyncCmd = [
        rsyncPath,
        "-avz",
        "-e",
        sshRsyncTransport,
        "--exclude",
        ".DS_Store",
        "--exclude",
        "*.tmp",
        "--exclude",
        "*.bak",
        "--exclude",
        ".venv/",
    ]
    if awsDryRun:
        rsyncCmd.extend(["--dry-run", "--itemize-changes"])

    rsyncCmd.extend(
        [
            f"{sourceProjectPath}/",
            f"{awsServer}:{remoteProjectPath}/",
        ]
    )

    print("Starting AWS sync for project directory...")
    print(f"  Local:  {sourceProjectPath}")
    print(f"  Remote: {awsServer}:{remoteProjectPath}")
    print(f"  SSH key: {awsSshKey}")
    print(f"  Binaries: rsync={rsyncPath}, ssh={sshPath}")
    process = subprocess.run(rsyncCmd, text=True, capture_output=True)
    if process.stdout:
        print(process.stdout.strip())
    if process.stderr:
        print(process.stderr.strip())
    if process.returncode != 0:
        raise RuntimeError(f"AWS rsync failed with code {process.returncode}")


def syncProjectsFolderToAws(
    projectsRoot: Path,
    awsServer: str,
    awsTarget: str,
    awsSshKey: Path,
    awsDryRun: bool,
) -> None:
    rsyncPath = resolveExecutable("rsync", ["/usr/bin/rsync"])
    sshPath = resolveExecutable("ssh", ["/usr/bin/ssh"])

    if not projectsRoot.exists() or not projectsRoot.is_dir():
        raise RuntimeError(f"Projects directory does not exist for sync: {projectsRoot}")

    remoteProjectsPath = f"{awsTarget.rstrip('/')}/projects"

    mkdirCmd = [
        sshPath,
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        "-i",
        str(awsSshKey),
        awsServer,
        f"mkdir -p {shlex.quote(remoteProjectsPath)}",
    ]
    mkdirProcess = subprocess.run(mkdirCmd, text=True, capture_output=True)
    if mkdirProcess.returncode != 0:
        raise RuntimeError(
            f"Failed to create AWS projects directory: {mkdirProcess.stderr.strip() or mkdirProcess.stdout.strip()}"
        )

    sshRsyncTransport = (
        f"{sshPath} -o BatchMode=yes -o ConnectTimeout=10 -i {shlex.quote(str(awsSshKey))}"
    )

    rsyncCmd = [
        rsyncPath,
        "-avz",
        "-e",
        sshRsyncTransport,
        "--exclude",
        ".DS_Store",
        "--exclude",
        "*.tmp",
        "--exclude",
        "*.bak",
        "--exclude",
        ".venv/",
    ]
    if awsDryRun:
        rsyncCmd.extend(["--dry-run", "--itemize-changes"])

    rsyncCmd.extend(
        [
            f"{projectsRoot}/",
            f"{awsServer}:{remoteProjectsPath}/",
        ]
    )

    print("Starting AWS sync for full projects directory...")
    print(f"  Local:  {projectsRoot}")
    print(f"  Remote: {awsServer}:{remoteProjectsPath}")
    print(f"  SSH key: {awsSshKey}")
    print(f"  Binaries: rsync={rsyncPath}, ssh={sshPath}")
    process = subprocess.run(rsyncCmd, text=True, capture_output=True)
    if process.stdout:
        print(process.stdout.strip())
    if process.stderr:
        print(process.stderr.strip())
    if process.returncode != 0:
        raise RuntimeError(f"AWS rsync failed with code {process.returncode}")


def validateProjectsFolderForAwsSync(projectsRoot: Path) -> None:
    if not projectsRoot.exists() or not projectsRoot.is_dir():
        raise RuntimeError(f"Projects directory does not exist: {projectsRoot}")

    projectDirs = sorted([path for path in projectsRoot.iterdir() if path.is_dir()])
    if not projectDirs:
        raise RuntimeError(f"Projects directory is empty: {projectsRoot}")

    requiredMetaFiles = ["project.json", "project_en.md", "project_nl.md"]
    validationErrors: list[str] = []

    for projectDir in projectDirs:
        for fileName in requiredMetaFiles:
            if not (projectDir / fileName).is_file():
                validationErrors.append(
                    f"{projectDir.name}: missing metadata file '{fileName}'"
                )

        versionArtifacts = sorted(projectDir.rglob("flash.json"))
        if not versionArtifacts:
            validationErrors.append(
                f"{projectDir.name}: no build output found (flash.json is missing)"
            )
            continue

        hasValidArtifactSet = False
        for flashJsonPath in versionArtifacts:
            artifactDir = flashJsonPath.parent
            if (artifactDir / "firmware.bin").is_file():
                hasValidArtifactSet = True
                break

        if not hasValidArtifactSet:
            validationErrors.append(
                f"{projectDir.name}: invalid build output (firmware.bin is missing next to flash.json)"
            )

    if validationErrors:
        errorLines = "\n  - " + "\n  - ".join(validationErrors)
        raise RuntimeError(
            "Projects directory is not correctly populated for --only-sync-aws:" + errorLines
        )


def detectBuildSystem(projectPath: Path) -> str:
    if (projectPath / "platformio.ini").is_file():
        return "platformio"

    cmakeFile = projectPath / "CMakeLists.txt"
    if cmakeFile.is_file():
        cmakeText = cmakeFile.read_text(encoding="utf-8", errors="ignore")
        if "project.cmake" in cmakeText or (projectPath / "sdkconfig").is_file():
            return "esp-idf"

    raise RuntimeError(
        "Build system not recognized: expected platformio.ini or an ESP-IDF CMakeLists.txt"
    )


def loadEspIdfFlasherArgs(buildDir: Path) -> dict:
    flasherArgsPath = buildDir / "flasher_args.json"
    if not flasherArgsPath.is_file():
        raise RuntimeError(f"ESP-IDF flash manifest not found: {flasherArgsPath}")
    try:
        payload = json.loads(flasherArgsPath.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Cannot read {flasherArgsPath}: {exc}") from exc
    if not isinstance(payload, dict) or not isinstance(payload.get("flash_files"), dict):
        raise RuntimeError(f"Invalid ESP-IDF flash manifest: {flasherArgsPath}")
    return payload


def resolveEspIdfBuildFile(buildDir: Path, fileValue: str) -> Path:
    candidate = Path(fileValue)
    if not candidate.is_absolute():
        candidate = buildDir / candidate
    return candidate.resolve()


def detectEspIdfTarget(buildDir: Path, flasherArgs: dict) -> str:
    descriptionPath = buildDir / "project_description.json"
    if descriptionPath.is_file():
        try:
            description = json.loads(descriptionPath.read_text(encoding="utf-8"))
            target = description.get("target")
            if isinstance(target, str) and target.strip():
                return sanitizePathSegment(target)
        except (OSError, json.JSONDecodeError):
            pass

    chip = flasherArgs.get("extra_esptool_args", {}).get("chip")
    if isinstance(chip, str) and chip.strip():
        return sanitizePathSegment(chip)

    return "esp32"


def findEspIdfPartitionsCsv(projectPath: Path, buildDir: Path) -> Path | None:
    candidates = [
        buildDir / "partition_table" / "partition-table.csv",
    ]

    #-- Custom partition tables (e.g. "partitions/radio_4mb.csv") are declared in sdkconfig
    sdkconfigPath = projectPath / "sdkconfig"
    if sdkconfigPath.is_file():
        sdkconfigText = sdkconfigPath.read_text(encoding="utf-8", errors="ignore")
        filenameMatch = partitionTableFilenamePattern.search(sdkconfigText)
        if filenameMatch:
            candidates.append(projectPath / filenameMatch.group(1))

    candidates.append(projectPath / "partitions.csv")

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    generated = sorted((buildDir / "partition_table").glob("*.csv"))
    return generated[0] if generated else None


def classifyEspIdfFlashFile(
    partitionKey: str,
    source: Path,
    partitions: dict[str, dict[str, str]] | None = None,
) -> str:
    normalizedKey = partitionKey.strip().lower()
    if normalizedKey == "bootloader":
        return "bootloader.bin"
    if normalizedKey in {"partition-table", "partition_table"}:
        return "partitions.bin"
    if normalizedKey == "app":
        return "firmware.bin"

    #-- ESP-IDF names filesystem images after the partition (e.g. "storage"), not the fs type
    partitionEntry = (partitions or {}).get(partitionKey) or (partitions or {}).get(normalizedKey)
    subtype = (partitionEntry.get("subtype") or "").strip().lower() if partitionEntry else ""
    if subtype in {"littlefs", "spiffs", "fatfs"}:
        return "storage.bin"

    name = source.name.lower()
    if "bootloader" in name:
        return "bootloader.bin"
    if "partition" in name and name.endswith(".bin"):
        return "partitions.bin"
    if "boot_app0" in name:
        return "boot_app0.bin"
    if "ota_data" in name or "phy_init" in name:
        return source.name
    if any(token in name for token in ("littlefs", "spiffs", "fatfs")):
        return "storage.bin"

    return source.name


def buildOffsetToPartitionNameMap(flasherArgs: dict) -> dict[str, str]:
    #-- flasher_args.json also carries per-partition entries keyed by partition name
    #-- (e.g. "bootloader", "partition-table", "app", plus any custom data partition
    #-- name such as "storage"), each with its own "offset"/"file" — that name is the
    #-- authoritative way to classify a flash file, since the data partition's name
    #-- and its build artifact's filename both vary per project.
    reservedKeys = {"write_flash_args", "flash_settings", "extra_esptool_args", "flash_files"}
    offsetToName: dict[str, str] = {}
    for key, value in flasherArgs.items():
        if key in reservedKeys or not isinstance(value, dict):
            continue
        offsetValue = value.get("offset")
        if isinstance(offsetValue, str) and offsetValue:
            offsetToName[offsetValue] = key
    return offsetToName


def findEspIdfExportScript() -> Path | None:
    candidates: list[Path] = []
    configuredIdfPath = os.environ.get("IDF_PATH")
    if configuredIdfPath:
        candidates.append(Path(configuredIdfPath).expanduser() / "export.sh")
    candidates.extend(sorted(
        (Path.home() / ".espressif").glob("*/esp-idf/export.sh"), reverse=True
    ))
    candidates.append(Path.home() / "esp" / "esp-idf" / "export.sh")

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    return None


def runEspIdfCommand(cmdArgs: list[str], cwd: Path, logLines: list[str]) -> None:
    #-- Run idf.py directly or bootstrap its environment via export.sh in a child shell
    idfExecutable = shutil.which("idf.py")
    if idfExecutable and os.environ.get("IDF_PATH"):
        runCommand([idfExecutable] + cmdArgs, cwd, logLines)
        return

    exportScript = findEspIdfExportScript()
    if exportScript is None:
        raise RuntimeError(
            "ESP-IDF was not found. Install ESP-IDF or activate it with export.sh."
        )
    logLines.append(f"Automatically activating ESP-IDF via {exportScript}")
    shellCommand = 'source "$1" >/dev/null && exec idf.py "${@:2}"'
    runCommand(
        ["/bin/zsh", "-c", shellCommand, "esp-idf-auto", str(exportScript)] + cmdArgs,
        cwd,
        logLines,
    )


def addOptionalEspIdfFilesystem(
    buildDir: Path,
    targetVersionDir: Path,
    partitions: dict[str, dict[str, str]],
    flashFiles: list[dict[str, str]],
    copiedSources: set[Path],
    logLines: list[str],
) -> None:
    candidates: list[Path] = []
    for pattern in ("*littlefs*.bin", "*spiffs*.bin", "*fatfs*.bin", "*storage*.bin"):
        candidates.extend(sorted(buildDir.rglob(pattern)))

    for source in candidates:
        resolved = source.resolve()
        if resolved in copiedSources or not source.is_file():
            continue

        offset = detectFilesystemOffset(partitions)
        if not offset:
            logLines.append(f"WARN: filesystem image found but no partition offset: {source}")
            return

        partitionName = next(
            (name for name, entry in partitions.items() if entry.get("offset") == offset), ""
        )
        destinationName = classifyEspIdfFlashFile(partitionName, source, partitions)
        shutil.copy2(source, targetVersionDir / destinationName)
        flashFiles.append({"offset": offset, "file": destinationName})
        logLines.append(f"Optional filesystem included: {source}")
        return


def buildEspIdfProject(projectPath: Path, targetProjectDir: Path, version: str) -> None:
    buildDir = projectPath / "build"
    logLines: list[str] = []
    try:
        runEspIdfCommand(["build"], projectPath, logLines)
    except RuntimeError as exc:
        errorText = str(exc)
        pythonCacheMismatch = (
            "project was configured with" in errorText
            and "Run 'idf.py fullclean'" in errorText
        )
        if not pythonCacheMismatch:
            raise
        logLines.append("Cached Python path mismatch detected; running idf.py fullclean")
        runEspIdfCommand(["fullclean"], projectPath, logLines)
        runEspIdfCommand(["build"], projectPath, logLines)

    flasherArgs = loadEspIdfFlasherArgs(buildDir)
    offsetToPartitionName = buildOffsetToPartitionNameMap(flasherArgs)
    boardName = detectEspIdfTarget(buildDir, flasherArgs)
    targetVersionDir = targetProjectDir / boardName / version
    targetVersionDir.mkdir(parents=True, exist_ok=True)

    partitionsCsvSource = findEspIdfPartitionsCsv(projectPath, buildDir)
    partitions: dict[str, dict[str, str]] = {}
    if partitionsCsvSource:
        shutil.copy2(partitionsCsvSource, targetVersionDir / "partitions.csv")
        partitions = parsePartitionsCsv(partitionsCsvSource)

    flashFiles: list[dict[str, str]] = []
    copiedSources: set[Path] = set()
    usedNames: set[str] = set()
    firmwareOffset = detectFirmwareOffset(partitions, "esp32").lower()
    flashManifest = flasherArgs["flash_files"]
    for rawOffset, fileValue in flashManifest.items():
        source = resolveEspIdfBuildFile(buildDir, str(fileValue))
        if not source.is_file():
            raise RuntimeError(f"ESP-IDF flash file does not exist: {source}")

        offset = str(rawOffset)
        partitionName = offsetToPartitionName.get(offset)
        if partitionName:
            destinationName = classifyEspIdfFlashFile(partitionName, source, partitions)
        elif offset.lower() == firmwareOffset:
            destinationName = "firmware.bin"
        else:
            destinationName = classifyEspIdfFlashFile("", source, partitions)
        if destinationName in usedNames:
            destinationName = source.name
        shutil.copy2(source, targetVersionDir / destinationName)
        flashFiles.append({"offset": offset, "file": destinationName})
        copiedSources.add(source.resolve())
        usedNames.add(destinationName)

    addOptionalEspIdfFilesystem(
        buildDir, targetVersionDir, partitions, flashFiles, copiedSources, logLines,
    )

    flashFiles.sort(key=lambda item: int(str(item["offset"]), 0))

    flashPayload = {
        "board": boardName,
        "soc": boardName,
        "version": version,
        "flash_files": flashFiles,
    }
    (targetVersionDir / "flash.json").write_text(
        json.dumps(flashPayload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    now = dt.datetime.now().isoformat(timespec="seconds")
    logBody = ["# Build log for ESP-IDF", "", f"Generated: {now}", ""] + logLines
    logBody.extend(["", f"Source manifest: {buildDir / 'flasher_args.json'}", ""])
    (targetVersionDir / "build_log.md").write_text("\n".join(logBody), encoding="utf-8")
    print(f"Completed ESP-IDF target '{boardName}': {targetVersionDir}")


def buildPlatformioProject(projectPath: Path, targetProjectDir: Path, version: str) -> None:
    if shutil.which("pio") is None:
        raise RuntimeError(
            "PlatformIO CLI not found. Install PlatformIO Core and ensure 'pio' is in PATH."
        )

    platformioIni = projectPath / "platformio.ini"
    workspaceDir = getWorkspaceDir(platformioIni, projectPath)
    platformioSections = parsePlatformioSections(platformioIni)
    envNotes = parseEnvNotes(platformioIni)

    envs = parseEnvs(platformioIni)
    if not envs:
        raise RuntimeError("No [env:...] sections found in platformio.ini")

    envBoardMap: dict[str, str] = {}
    envSocMap: dict[str, str] = {}
    boardCounts: dict[str, int] = {}
    for env in envs:
        boardName = resolveEnvBoardName(platformioSections, env)
        platformName = resolveEnvPlatformName(platformioSections, env)
        socFamily = detectSocFamily(boardName, platformName)
        envBoardMap[env] = boardName
        envSocMap[env] = socFamily
        boardCounts[boardName] = boardCounts.get(boardName, 0) + 1

    print(f"Environments: {', '.join(envs)}")
    print("Boards per environment:")
    for env in envs:
        print(f"  - {env} -> {envBoardMap[env]} ({envSocMap[env]})")
    print(f"Workspace dir: {workspaceDir}")

    for env in envs:
        boardName = envBoardMap[env]
        socFamily = envSocMap[env]

        envDirName = sanitizePathSegment(env)
        envDir = targetProjectDir / envDirName
        envDir.mkdir(parents=True, exist_ok=True)
        envNote = (envNotes.get(env) or "").strip()
        if envNote:
            (envDir / "type_note.txt").write_text(envNote + "\n", encoding="utf-8")

        envVersionDir = envDir / boardName / version
        envVersionDir.mkdir(parents=True, exist_ok=True)

        logLines: list[str] = []
        runCommand(["pio", "run", "-e", env], projectPath, logLines)

        if (projectPath / "data").is_dir():
            try:
                runCommand(["pio", "run", "-e", env, "-t", "buildfs"], projectPath, logLines)
            except RuntimeError as exc:
                logLines.append(f"WARN: buildfs niet gelukt voor {env}: {exc}")

        envPartitionsSource = resolveEnvPartitionsSource(
            projectPath,
            platformioSections,
            env,
            socFamily,
        )
        envLdscriptSource = resolveEnvLdscriptSource(
            projectPath,
            platformioSections,
            env,
            socFamily,
        )

        collectAndCopyArtifacts(
            projectPath,
            workspaceDir,
            env,
            boardName,
            socFamily,
            envVersionDir,
            envPartitionsSource,
            envLdscriptSource,
            version,
            logLines,
        )
        print(f"Completed for env '{env}': {envVersionDir}")


def main() -> int:
    parser = argparse.ArgumentParser(
        usage="%(prog)s [project] [--sync-aws | --only-sync-aws] [--aws-dry-run]",
        description=f"createProjectStructure.py {scriptVersion}\nCreate projects structure from a PlatformIO or ESP-IDF project.",
    )
    parser.add_argument(
        "project",
        nargs="?",
        help="Path to PlatformIO or ESP-IDF project",
    )
    syncModeGroup = parser.add_mutually_exclusive_group()
    syncModeGroup.add_argument(
        "--sync-aws",
        action="store_true",
        help="Sync generated projects/<project> directory to AWS (add/update only, never delete)",
    )
    syncModeGroup.add_argument(
        "--only-sync-aws",
        action="store_true",
        help="Skip build and only sync the full local projects directory to AWS",
    )
    parser.add_argument(
        "--aws-dry-run",
        action="store_true",
        help="Show AWS rsync changes without actually copying",
    )

    if len(sys.argv) == 1:
        parser.print_help()
        return 0

    args = parser.parse_args()

    if not args.project:
        parser.print_help()
        return 0

    projectPath = Path(args.project).expanduser().resolve()
    if not projectPath.exists() or not projectPath.is_dir():
        raise SystemExit(f"Invalid project path: {projectPath}")

    outputRoot = projectPath
    projectsRoot = outputRoot / "projects"

    if args.only_sync_aws:
        awsSshKey = Path(defaultAwsSshKey).expanduser().resolve()
        if not awsSshKey.exists():
            raise SystemExit(f"SSH key not found: {awsSshKey}")
        print(f"Validating projects directory: {projectsRoot}")
        validateProjectsFolderForAwsSync(projectsRoot)
        syncProjectsFolderToAws(
            projectsRoot=projectsRoot,
            awsServer=defaultAwsServer,
            awsTarget=defaultAwsTarget,
            awsSshKey=awsSshKey,
            awsDryRun=args.aws_dry_run,
        )
        print("Projects directory synchronized successfully.")
        return 0

    os.chdir(projectPath)
    buildSystem = detectBuildSystem(projectPath)

    version = detectVersion(projectPath)
    projectName = projectPath.name

    targetProjectDir = projectsRoot / projectName
    if targetProjectDir.exists():
        print(f"Removing existing project directory: {targetProjectDir}")
        shutil.rmtree(targetProjectDir)
    targetProjectDir.mkdir(parents=True, exist_ok=True)

    projectMetaDataDir = ensureProjectMetaDataDefaults(projectPath)
    copyProjectMetaData(projectMetaDataDir, targetProjectDir)

    print(f"createProjectStructure.py {scriptVersion}")
    print(f"Project: {projectName}")
    print(f"Build system: {buildSystem}")
    print(f"Version: {version}")
    print(f"Output: {targetProjectDir}")

    if buildSystem == "platformio":
        buildPlatformioProject(projectPath, targetProjectDir, version)
    else:
        buildEspIdfProject(projectPath, targetProjectDir, version)

    if args.sync_aws:
        awsSshKey = Path(defaultAwsSshKey).expanduser().resolve()
        if not awsSshKey.exists():
            raise SystemExit(f"SSH key not found: {awsSshKey}")
        syncProjectToAws(
            projectsRoot=projectsRoot,
            projectName=projectName,
            awsServer=defaultAwsServer,
            awsTarget=defaultAwsTarget,
            awsSshKey=awsSshKey,
            awsDryRun=args.aws_dry_run,
        )

    print("Structure created successfully.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit("Aborted by user.")
    