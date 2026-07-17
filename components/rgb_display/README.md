Замените папку components/rgb_display содержимым этой папки.

После замены выполните:

idf.py fullclean
idf.py build
idf.py flash monitor

Версия использует два framebuffer и ждёт VSYNC перед повторным использованием
буфера. После переключения текущий кадр копируется в новый back buffer, чтобы
частичная перерисовка ui_home_dynamic не смешивала старые и новые строки.
