idf_component_register( SRCS
                            "mc_wifi.c"
                        INCLUDE_DIRS
                            "include"
                        PRIV_REQUIRES
                            nvs_flash

                            mc_assert
                            )
