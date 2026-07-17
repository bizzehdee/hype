#include "file_io.h"

EFI_STATUS hype_file_locate_root(EFI_HANDLE image_handle, EFI_BOOT_SERVICES *bs,
                                  EFI_FILE_PROTOCOL **out_root) {
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID simple_fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_STATUS status;

    status = bs->HandleProtocol(image_handle, &loaded_image_guid, (void **)&loaded_image);
    if (status != EFI_SUCCESS) {
        return status;
    }

    status = bs->HandleProtocol(loaded_image->DeviceHandle, &simple_fs_guid, (void **)&fs);
    if (status != EFI_SUCCESS) {
        return status;
    }

    return fs->OpenVolume(fs, out_root);
}

EFI_STATUS hype_file_get_size(EFI_FILE_PROTOCOL *root, EFI_BOOT_SERVICES *bs, CHAR16 *path,
                               UINT64 *out_size) {
    EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status;
    UINTN info_size = 0;
    void *info_buf = 0;
    EFI_FILE_INFO_HEADER *info;

    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        return status;
    }

    status = file->GetInfo(file, &file_info_guid, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL) {
        file->Close(file);
        return (status == EFI_SUCCESS) ? EFI_ABORTED : status;
    }

    status = bs->AllocatePool(EfiLoaderData, info_size, &info_buf);
    if (status != EFI_SUCCESS) {
        file->Close(file);
        return status;
    }

    status = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
    if (status != EFI_SUCCESS) {
        bs->FreePool(info_buf);
        file->Close(file);
        return status;
    }

    info = (EFI_FILE_INFO_HEADER *)info_buf;
    *out_size = info->FileSize;

    bs->FreePool(info_buf);
    file->Close(file);
    return EFI_SUCCESS;
}

EFI_STATUS hype_file_read_into(EFI_FILE_PROTOCOL *root, CHAR16 *path, void *buffer, UINTN buffer_size) {
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status;
    UINTN read_size = buffer_size;

    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        return status;
    }

    status = file->Read(file, &read_size, buffer);
    file->Close(file);
    if (status != EFI_SUCCESS) {
        return status;
    }
    if (read_size != buffer_size) {
        return EFI_ABORTED;
    }
    return EFI_SUCCESS;
}

EFI_STATUS hype_file_write_new(EFI_FILE_PROTOCOL *root, CHAR16 *path, const void *buffer, UINTN size) {
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status;
    UINTN write_size = size;

    /* Open (creating if absent) and overwrite from offset 0. Deliberately
     * does NOT delete+recreate each call: this is written repeatedly to
     * the same growing log file (periodic flush), and the delete+create
     * churn is exactly what trips fragile FAT write paths -- QEMU's vvfat
     * asserts, and picky firmware drivers can choke too. The caller writes
     * a monotonically GROWING buffer, so overwriting from 0 never leaves a
     * stale tail within a run; cross-run stale data is cleared once up
     * front via hype_file_delete(). */
    status = root->Open(root, &file, path,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        return status;
    }

    status = file->Write(file, &write_size, (void *)buffer);
    if (status == EFI_SUCCESS && file->Flush != 0) {
        file->Flush(file);
    }
    file->Close(file);
    if (status != EFI_SUCCESS) {
        return status;
    }
    if (write_size != size) {
        return EFI_ABORTED;
    }
    return EFI_SUCCESS;
}

EFI_STATUS hype_file_delete(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = root->Open(root, &file, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (status != EFI_SUCCESS) {
        return status; /* not present (or unopenable) -- nothing to delete */
    }
    return file->Delete(file); /* Delete() closes the handle too */
}
