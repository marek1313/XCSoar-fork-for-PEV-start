import zipfile
import os
import shutil

input_zip = "xcsoarpev.aab" 
output_zip = "xcsoarpev_aligned.aab"

PATH1_PREFIX = "base/lib"

with zipfile.ZipFile(input_zip, "r") as zin, \
     zipfile.ZipFile(output_zip, "w") as zout:

    for info in zin.infolist():
        name = info.filename

        # Odczytaj dane pliku ze starego ZIP-a
        with zin.open(info, "r") as src:
            data = src.read()

        # Ustal metodę kompresji na podstawie ścieżki
        if not name.startswith(PATH1_PREFIX):
            compression = zipfile.ZIP_DEFLATED   # kompresja
        else:
            compression = zipfile.ZIP_STORED     # bez kompresji

        # Utwórz nowe ZipInfo, żeby móc nadpisać compress_type
        new_info = zipfile.ZipInfo(filename=name,
                                   date_time=info.date_time)
        new_info.external_attr = info.external_attr
        new_info.comment = info.comment
        new_info.create_system = info.create_system

        # Zapisz dane do nowego ZIP-a z wybraną metodą kompresji
        zout.writestr(new_info, data, compress_type=compression)

