#define _POSIX_C_SOURCE 200809L

#include "installer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <minizip/unzip.h>
#include <switch.h>

#define SD_DEVICE "sdmc:"
#define SWITCH_DIRECTORY SD_DEVICE "/switch"

enum { path_limit = 512, component_limit = 120, depth_limit = 16, zip_file_limit = 40000 };
static const uint64_t expanded_limit = UINT64_C(2) * 1024 * 1024 * 1024;

typedef enum { VERIFIED, WRONG_FILE, CHECK_FAILED, CHECK_CANCELLED } Verification;

static bool make_directory(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return true;
    struct stat info;
    return errno == EEXIST && lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool suffix(const char *name, const char *extension)
{
    const size_t length = strlen(name);
    const size_t suffix_length = strlen(extension);
    return length > suffix_length && strcasecmp(name + length - suffix_length, extension) == 0;
}

static bool safe_component(const char *component, size_t length)
{
    if (!length || length > component_limit || component[0] == ' ' ||
        component[length - 1] == '.' || component[length - 1] == ' ')
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = component[i];
        if (ch < 32 || ch == 127 || strchr("\\/:*?\"<>|", ch))
            return false;
    }
    return true;
}

static bool safe_relative_path(const char *path)
{
    if (!path || strlen(path) >= path_limit)
        return false;
    unsigned depth = 0;
    const char *component = path;
    for (;;) {
        const char *end = strchr(component, '/');
        const size_t length = end ? (size_t)(end - component) : strlen(component);
        if (++depth > depth_limit || !safe_component(component, length))
            return false;
        if (!end)
            return true;
        component = end + 1;
    }
}

static bool reserved_folder(const char *name, size_t length)
{
    const char *reserved[] = {
        "atmosphere", "bootloader", "kaihen", "Nintendo", "sept", "switch2homebrew-store",
    };
    for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
        if (length == strlen(reserved[i]) && strncasecmp(name, reserved[i], length) == 0)
            return true;
    }
    return false;
}

static bool reserved_file(const char *name)
{
    const char *reserved[] = {"kaihen.nro", "boot.dat", "payload.bin", "prod.keys", "title.keys"};
    for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
        if (strcasecmp(name, reserved[i]) == 0)
            return true;
    }
    return false;
}

static bool valid_target(const StorePackage *package)
{
    if (!package->target || strlen(package->target) >= path_limit - 32)
        return false;
    static const char prefix[] = SWITCH_DIRECTORY "/";
    if (strncmp(package->target, prefix, sizeof(prefix) - 1) != 0)
        return package->kind == STORE_PACKAGE_ZIP && strcmp(package->target, SD_DEVICE "/retroarch") == 0;

    const char *name = package->target + sizeof(prefix) - 1;
    if (!safe_relative_path(name))
        return false;
    const char *slash = strchr(name, '/');
    const size_t length = slash ? (size_t)(slash - name) : strlen(name);
    if (reserved_folder(name, length))
        return false;
    if (package->kind == STORE_PACKAGE_NRO) {
        const char *filename = slash ? slash + 1 : name;
        return !strchr(filename, '/') && suffix(filename, ".nro") && !reserved_file(filename);
    }
    return package->kind == STORE_PACKAGE_ZIP && !slash;
}

static bool valid_package(const StorePackage *package)
{
    if (!package || !package->url || !package->url[0] || !valid_target(package) ||
        package->size > STORE_DOWNLOAD_LIMIT)
        return false;
    static const uint8_t empty_hash[32] = {0};
    if (memcmp(package->sha256, empty_hash, sizeof(empty_hash)) == 0)
        return false;
    if (package->kind != STORE_PACKAGE_ZIP)
        return true;
    const char *root = package->archive_root;
    const char *secondary = package->archive_secondary_root;
    return safe_relative_path(package->marker) && suffix(package->marker, ".nro") &&
        (!root || !root[0] || safe_relative_path(root)) &&
        (!secondary || !secondary[0] || (root && root[0] && safe_relative_path(secondary) &&
                                         strcmp(root, secondary) != 0));
}

static bool mkdir_parents(char *path, size_t start)
{
    for (char *separator = strchr(path + start, '/'); separator;
         separator = strchr(separator + 1, '/')) {
        *separator = '\0';
        const bool created = make_directory(path);
        *separator = '/';
        if (!created)
            return false;
    }
    return true;
}

static bool unique_path(char path[path_limit], const char *parent, bool directory, int *descriptor)
{
    const unsigned token = (unsigned)armGetSystemTick();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const int length = snprintf(path, path_limit, "%s/.s2h-%08x-%u%s", parent,
                                    token, attempt, directory ? "-files" : ".part");
        if (length < 0 || length >= path_limit)
            return false;
        if (directory) {
            if (mkdir(path, 0777) == 0)
                return true;
        } else {
            *descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
            if (*descriptor >= 0)
                return true;
        }
        if (errno != EEXIST)
            return false;
    }
    return false;
}

static void remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0)
        return;
    if (!S_ISDIR(info.st_mode)) {
        unlink(path);
        return;
    }
    DIR *directory = opendir(path);
    if (!directory)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[path_limit];
        const int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length > 0 && (size_t)length < sizeof(child))
            remove_tree(child);
    }
    closedir(directory);
    rmdir(path);
}

static Verification verify_digest(FILE *stream, const StorePackage *package, const atomic_bool *cancel)
{
    struct stat info;
    if (fstat(fileno(stream), &info) != 0 || fseek(stream, 0, SEEK_SET) != 0)
        return CHECK_FAILED;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || (uint64_t)info.st_size > STORE_DOWNLOAD_LIMIT ||
        (package->size && (uint64_t)info.st_size != package->size))
        return WRONG_FILE;

    uint8_t buffer[32 * 1024];
    uint8_t digest[SHA256_HASH_SIZE];
    Sha256Context context;
    sha256ContextCreate(&context);
    uint64_t bytes_read = 0;
    const uint64_t length = (uint64_t)info.st_size;

    while (bytes_read < length) {
        if (atomic_load(cancel))
            return CHECK_CANCELLED;
        const uint64_t remaining = length - bytes_read;
        const size_t count = fread(buffer, 1, remaining < sizeof buffer ? (size_t)remaining : sizeof buffer, stream);
        if (count == 0)
            return CHECK_FAILED;
        sha256ContextUpdate(&context, buffer, count);
        bytes_read += count;
    }
    sha256ContextGetHash(&context, digest);
    return memcmp(digest, package->sha256, sizeof digest) == 0 ? VERIFIED : WRONG_FILE;
}

static bool is_nro(const char *path)
{
    FILE *input = fopen(path, "rb");
    if (!input)
        return false;
    char magic[4];
    const bool matched = fseek(input, 0x10, SEEK_SET) == 0 &&
        fread(magic, 1, sizeof(magic), input) == sizeof(magic) &&
        memcmp(magic, "NRO0", sizeof(magic)) == 0;
    fclose(input);
    return matched;
}

static bool package_data_path(const char *name)
{
    char path[path_limit];
    strcpy(path, name);
    char *component = path;
    for (char *slash = strchr(component, '/'); slash; slash = strchr(component, '/')) {
        *slash = '\0';
        if (reserved_folder(component, strlen(component)))
            return false;
        component = slash + 1;
    }
    if (reserved_folder(component, strlen(component)))
        return false;
    const char *blocked_extensions[] = {
        ".exe", ".msi", ".bat", ".cmd", ".dll", ".ps1", ".nsp", ".nsz", ".xci", ".xcz", ".nca",
        ".rom", ".iso", ".chd", ".cue", ".cso", ".rvz", ".wbfs", ".wad",
        ".nes", ".sfc", ".smc", ".n64", ".z64", ".v64", ".gb", ".gbc", ".gba", ".nds", ".3ds", ".cia",
    };
    for (unsigned i = 0; i < sizeof blocked_extensions / sizeof blocked_extensions[0]; ++i) {
        if (suffix(component, blocked_extensions[i]))
            return false;
    }
    return !reserved_file(component);
}

static const char *relative_to_archive_root(const char *path, const char *root)
{
    if (!root || !root[0])
        return path;
    const size_t length = strlen(root);
    return strncmp(path, root, length) == 0 && path[length] == '/' ? path + length + 1 : NULL;
}

static bool extract_file(unzFile archive, const char *path, uint64_t expected, InstallJob *job)
{
    if (unzOpenCurrentFile(archive) != UNZ_OK)
        return store_install_error(&job->error, "Could not open an entry in the ZIP file.",
                                                "No se pudo abrir una entrada del ZIP.");

    const int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    FILE *output = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
    if (!output) {
        if (descriptor >= 0)
            close(descriptor);
        unzCloseCurrentFile(archive);
        return store_install_error(&job->error, "Could not create a file from the ZIP archive (errno %d).",
                                                "No se pudo crear un archivo del ZIP (errno %d).", errno);
    }

    bool ok = true;
    uint64_t total = 0;
    uint8_t buffer[16 * 1024];
    while (!atomic_load(&job->cancel)) {
        const int count = unzReadCurrentFile(archive, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0 || (uint64_t)count > expected - total ||
            fwrite(buffer, 1, (size_t)count, output) != (size_t)count) {
            ok = false;
            break;
        }
        total += (uint64_t)count;
    }
    if (fclose(output) != 0)
        ok = false;
    if (unzCloseCurrentFile(archive) != UNZ_OK)
        ok = false;
    return (ok && total == expected && !atomic_load(&job->cancel)) ||
        store_install_error(&job->error, "Could not extract and verify a file from the ZIP archive.",
                                         "No se pudo descomprimir y verificar un archivo del ZIP.");
}

static bool extract_entry(unzFile archive, const StorePackage *package, const char *staging,
                          InstallJob *job, uint64_t *expanded, bool *has_marker)
{
    unz_file_info64 info;
    char name[path_limit];
    if (unzGetCurrentFileInfo64(archive, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK ||
        info.size_filename == 0 || info.size_filename >= sizeof(name))
        return store_install_error(&job->error, "The ZIP archive contains an invalid filename.",
                                                "El ZIP contiene un nombre de archivo inválido.");
    name[info.size_filename] = '\0';
    if (strlen(name) != info.size_filename)
        return store_install_error(&job->error, "The ZIP archive contains an invalid filename.",
                                                "El ZIP contiene un nombre de archivo inválido.");

    const bool trailing_slash = name[info.size_filename - 1] == '/';
    const unsigned type = (unsigned)(info.external_fa >> 16) & 0170000u;
    const bool directory = trailing_slash || type == 0040000u || (info.external_fa & 0x10u);
    if (trailing_slash)
        name[info.size_filename - 1] = '\0';
    if (!safe_relative_path(name) || (type && type != (directory ? 0040000u : 0100000u)) ||
        (info.flag & 1u) || (info.compression_method != 0 && info.compression_method != Z_DEFLATED) ||
        (directory && info.uncompressed_size))
        return store_install_error(&job->error, "The ZIP archive contains unsupported paths or file types.",
                                                "El ZIP contiene rutas o tipos de archivo incompatibles.");

    const char *relative = relative_to_archive_root(name, package->archive_root);
    if (!relative && package->archive_secondary_root && package->archive_secondary_root[0])
        relative = relative_to_archive_root(name, package->archive_secondary_root);
    if (!relative)
        return true;
    if (!package_data_path(relative) || info.uncompressed_size > expanded_limit - *expanded)
        return store_install_error(&job->error, "The ZIP archive exceeds the installation limits.",
                                                "El ZIP supera los límites del paquete instalable.");

    char destination[path_limit];
    const int length = snprintf(destination, sizeof(destination), "%s/%s", staging, relative);
    if (length < 0 || (size_t)length >= sizeof(destination) || !mkdir_parents(destination, strlen(staging) + 1))
        return store_install_error(&job->error, "Could not prepare a folder from the ZIP archive.",
                                                "No se pudo preparar una carpeta del ZIP.");
    if (directory)
        return make_directory(destination) || store_install_error(&job->error,
            "Could not create a folder from the ZIP archive.",
            "No se pudo crear una carpeta del ZIP.");
    if (!extract_file(archive, destination, (uint64_t)info.uncompressed_size, job))
        return false;

    *expanded += (uint64_t)info.uncompressed_size;
    if (strcmp(relative, package->marker) == 0)
        *has_marker = is_nro(destination);
    return true;
}

static bool extract_zip(const char *archive_path, const char *staging, InstallJob *job)
{
    unzFile archive = unzOpen64(archive_path);
    if (!archive)
        return store_install_error(&job->error, "Could not open the downloaded file as a ZIP archive.",
                                                "El archivo descargado no se pudo abrir como ZIP.");

    unz_global_info64 info;
    bool ok = unzGetGlobalInfo64(archive, &info) == UNZ_OK && info.number_entry > 0 &&
        info.number_entry <= zip_file_limit && unzGoToFirstFile(archive) == UNZ_OK;
    if (!ok)
        store_install_error(&job->error, "The ZIP archive contains an unsupported number of entries.",
                                         "El ZIP contiene un número de entradas incompatible.");

    uint64_t expanded = 0;
    bool has_marker = false;
    for (uint64_t entry = 0; ok && entry < info.number_entry; ++entry) {
        if (atomic_load(&job->cancel)) {
            ok = false;
            break;
        }
        ok = extract_entry(archive, job->package, staging, job, &expanded, &has_marker);
        if (ok && entry + 1 < info.number_entry && unzGoToNextFile(archive) != UNZ_OK)
            ok = store_install_error(&job->error, "The ZIP archive's file list is incomplete.",
                                                  "La lista de archivos del ZIP está incompleta.");
    }
    unzClose(archive);
    return ok && (has_marker || store_install_error(&job->error,
        "The ZIP archive does not contain the expected NRO.",
        "El ZIP no contiene el NRO esperado."));
}

static InstallPhase existing_package(const StorePackage *package, InstallJob *job)
{
    if (package->kind != STORE_PACKAGE_NRO)
        return CONFLICT;
    FILE *input = fopen(package->target, "rb");
    if (!input)
        return CONFLICT;
    const Verification status = verify_digest(input, package, &job->cancel);
    fclose(input);
    if (status == CHECK_CANCELLED)
        return CANCELLED;
    return status == VERIFIED ? ALREADY_INSTALLED : CONFLICT;
}

void install_package(void *argument)
{
    InstallJob *job = argument;
    const StorePackage *package = job->package;
    InstallPhase outcome = FAILED;
    char downloaded_path[path_limit] = "";
    char staging[path_limit] = "";
    bool owns_download = false;
    bool owns_staging = false;
    FILE *download = NULL;

    atomic_store(&job->downloaded, 0);
    atomic_store(&job->total, 0);
    atomic_store(&job->phase, PREPARING);
    job->error = (StoreInstallError){0};
    if (!valid_package(package)) {
        store_install_error(&job->error, "This package has no verified installation.",
                                         "Este paquete no tiene una instalación verificada.");
        goto finish;
    }
    atomic_store(&job->total, (unsigned)package->size);

    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) {
        store_install_error(&job->error, "Could not access the microSD card.",
                                         "No se pudo acceder a la microSD.");
        goto finish;
    }
    if (atomic_load(&job->cancel))
        goto finish;

    struct stat info;
    if (lstat(package->target, &info) == 0) {
        outcome = S_ISREG(info.st_mode) ? existing_package(package, job) : CONFLICT;
        goto finish;
    }
    if (errno != ENOENT) {
        store_install_error(&job->error, "Could not check the installation destination (errno %d).",
                                         "No se pudo comprobar el destino (errno %d).", errno);
        goto finish;
    }

    char parent[path_limit];
    strcpy(parent, package->target);
    *strrchr(parent, '/') = '\0';
    if (strcmp(parent, SD_DEVICE) != 0 &&
        (!mkdir_parents(parent, sizeof(SD_DEVICE)) || !make_directory(parent))) {
        store_install_error(&job->error, "Could not prepare the folder on the microSD card.",
                                         "No se pudo preparar la carpeta de la microSD.");
        goto finish;
    }
    int descriptor;
    if (!unique_path(downloaded_path, parent, false, &descriptor)) {
        store_install_error(&job->error, "Could not create the temporary file (errno %d).",
                                         "No se pudo crear el archivo temporal (errno %d).", errno);
        goto finish;
    }
    owns_download = true;
    download = fdopen(descriptor, "w+b");
    if (!download) {
        close(descriptor);
        store_install_error(&job->error, "Could not open the temporary download.",
                                         "No se pudo abrir la descarga temporal.");
        goto finish;
    }

    atomic_store(&job->phase, DOWNLOADING);
    if (!download_asset(download, package->url, package->size, &job->downloaded, &job->total,
                        &job->cancel, &job->error))
        goto finish;

    atomic_store(&job->phase, VERIFYING);
    if (fflush(download) != 0 || fsync(fileno(download)) != 0) {
        store_install_error(&job->error, "Could not save the download (errno %d).",
                                         "No se pudo guardar la descarga (errno %d).", errno);
        goto finish;
    }
    const Verification verified = verify_digest(download, package, &job->cancel);
    if (verified != VERIFIED) {
        if (verified == CHECK_FAILED)
            store_install_error(&job->error, "Could not read the download from the microSD card.",
                                 "No se pudo leer la descarga desde la microSD.");
        else
            store_install_error(&job->error, "The download does not match the published SHA-256.",
                                 "El SHA-256 de la descarga no coincide con el publicado.");
        goto finish;
    }
    const int closed = fclose(download);
    download = NULL;
    if (closed != 0) {
        store_install_error(&job->error, "Could not close the temporary file (errno %d).",
                                         "No se pudo cerrar el archivo temporal (errno %d).", errno);
        goto finish;
    }

    if (package->kind == STORE_PACKAGE_ZIP) {
        if (!unique_path(staging, parent, true, NULL)) {
            store_install_error(&job->error, "Could not create the temporary folder (errno %d).",
                                             "No se pudo crear la carpeta temporal (errno %d).", errno);
            goto finish;
        }
        owns_staging = true;
        atomic_store(&job->phase, SAVING);
        if (!extract_zip(downloaded_path, staging, job))
            goto finish;
    } else if (!is_nro(downloaded_path)) {
        store_install_error(&job->error, "The download does not contain a valid NRO.",
                                         "La descarga no contiene un NRO válido.");
        goto finish;
    }

    if (atomic_load(&job->cancel))
        goto finish;
    if (lstat(package->target, &info) == 0) {
        outcome = CONFLICT;
        goto finish;
    }
    if (errno != ENOENT) {
        store_install_error(&job->error, "Could not check the installation destination (errno %d).",
                                         "No se pudo comprobar el destino (errno %d).", errno);
        goto finish;
    }

    atomic_store(&job->phase, SAVING);
    const char *destination = package->target + sizeof(SD_DEVICE) - 1;
    const Result installed = package->kind == STORE_PACKAGE_ZIP
        ? fsFsRenameDirectory(sd, staging + sizeof(SD_DEVICE) - 1, destination)
        : fsFsRenameFile(sd, downloaded_path + sizeof(SD_DEVICE) - 1, destination);
    if (R_FAILED(installed)) {
        if (lstat(package->target, &info) == 0)
            outcome = CONFLICT;
        else
            store_install_error(&job->error, "Could not place the package on the microSD card (0x%08x).",
                                             "La microSD no pudo colocar el paquete (0x%08x).", (unsigned)installed);
        goto finish;
    }
    if (package->kind == STORE_PACKAGE_ZIP)
        owns_staging = false;
    else
        owns_download = false;
    const Result committed = fsdevCommitDevice("sdmc");
    if (R_FAILED(committed))
        store_install_error(&job->error, "The microSD card reported a warning while saving (0x%08x).",
                                         "La confirmación de escritura dio un aviso (0x%08x).", (unsigned)committed);
    outcome = INSTALLED;

finish:
    if (download)
        fclose(download);
    if (owns_download)
        unlink(downloaded_path);
    if (owns_staging)
        remove_tree(staging);
    if (outcome != INSTALLED && atomic_load(&job->cancel))
        outcome = CANCELLED;
    atomic_store(&job->phase, outcome);
}
