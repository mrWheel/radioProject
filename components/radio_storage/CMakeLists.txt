idf_component_register(SRCS "radio_storage.c" INCLUDE_DIRS "include" REQUIRES littlefs)
littlefs_create_partition_image(storage ../../littlefs FLASH_IN_PROJECT)

