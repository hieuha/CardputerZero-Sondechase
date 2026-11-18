# SondeChase

[English](README.md) | **Tiếng Việt**

Ứng dụng thu, giải mã radiosonde RS41 và hỗ trợ chase trên M5Stack CardputerZero
(Raspberry Pi CM0, LCD 320×170, bàn phím 46 phím) với RTL-SDR v4 và GPS.

Phiên bản 0.3.0. Xem mục "Trạng thái kiểm thử" trước khi dùng: UI đã biên dịch và render với LVGL 9.5
trên PC; các đường phần cứng chưa được chạy trên thiết bị thật.

## Kiến trúc

```
┌────────────┐    ┌────────┐ IQ 48 kHz ┌───────────┐ bits 4800 Bd ┌─────────┐
│ RTL-SDR v4 ├─┬─►│ rtl_fm ├──────────►│ fsk_demod ├─────────────►│ rs41mod ├───┐ frame JSON
└────────────┘ │  └────────┘           └───────────┘              └─────────┘   │
               │  ┌───────────┐                                                 │
               └─►│ rtl_power ├── CSV 400–406 MHz, chỉ khi quét ────────────────┤ phổ
                  └───────────┘                                                 │
┌────────────┐    ┌──────┐                                                      │
│ GPS        ├───►│ gpsd ├── JSON, TCP 2947 ────────────────────────────────────┤ vị trí
└────────────┘    └──────┘                                                      │
┌────────────┐                                                                  │
│ Từ kế      ├── IIO sysfs ─────────────────────────────────────────────────────┤ hướng
└────────────┘                                                                  │
    ┌───────────────────────────────────────────────────────────────────────────▼──────┐
    │  sondechase  ·  C, LVGL 9.5, /dev/fb0 + evdev                                    │
    │                                                                                  │
    │  app.c    máy trạng thái IDLE / DECODE / SCAN                                    │
    │  geo.c    khoảng cách, bearing, elevation, ISA, dự đoán điểm rơi                 │
    │  track.c  lịch sử bay, phát hiện bóng nổ, beeper, audio, tile                    │
    │  io.c     gpsd, heading, phổ rtl_power                                           │
    │  ui.c     7 màn hình + cảnh báo, vẽ bằng line / rect / label                     │
    └───────────────────────────────────────────────────────────────────────────┬──────┘
                                                                                │ PCM 16 kHz qua pipe → PipeWire
                                                                            ┌───▼───┐
                                                                            │ pacat │
                                                                            └───────┘
```

`radiosonde_auto_rx` không chạy. App chỉ dùng hai binary `rs41mod` và `fsk_demod` build từ
source tree của dự án đó, và tự làm phần điều khiển SDR, quét, tracking, dự đoán, UI.

RTL-SDR chỉ phục vụ một tiến trình tại một thời điểm: khi quét (`rtl_power`), pipeline giải mã
dừng; sau khi chọn peak, pipeline giải mã khởi động lại ở tần số mới.

## Màn hình và phím

| Phím | Chức năng |
|---|---|
| `1` Track | La bàn HUD, khoảng cách, bearing, elevation, điểm rơi dự đoán |
| `2` Map | Tile nạp sẵn, vị trí máy, sonde, vệt bay, điểm rơi. `Up/Down` zoom, `C` đổi chế độ bám (both / device / sonde) |
| `3` Graph | `Left/Right` đổi trang: Skew-T log-P, độ cao + tốc độ đứng theo thời gian |
| `4` Scan | Phổ 400–406 MHz. `Left/Right` chọn peak, `Enter` tune, `S` quét lại |
| `5` Info | Sonde, thu nhận, thiết bị, một cặp tọa độ đích |
| `F` | Nhập tần số (kHz) |
| `M` | Tắt / bật tiếng beeper |
| `R` / `X` / `Q` | Khởi động lại decoder / dừng / thoát |

Cảnh báo bóng nổ hiện đè lên mọi màn hình: `Enter` mở Map, `Esc` đóng.

**Trạng thái thiếu phần cứng**

- Không có SDR trên USB (VID:PID `0bda:2838` hoặc `0bda:2832`): màn hình chặn "No SDR device found".
  App kiểm tra lại USB mỗi 2 s và tự tiếp tục giải mã khi thiết bị xuất hiện. Tắt kiểm tra: `sdr_check = 0`.
- Không có GPS fix: việc giải mã sonde không bị ảnh hưởng. Thanh trên cùng hiện `GPS lost` (đã từng có fix)
  hoặc `No GPS`. Màn hình Track thay khoảng cách, bearing, elevation bằng độ cao và tọa độ tuyệt đối của
  sonde và của điểm rơi dự đoán. Map bám theo sonde. Beeper không kêu.

## Thuật toán

**Dự đoán điểm rơi** (`geo.c`, `predict_landing`)

- Tốc độ rơi dưới dù ở vận tốc cuối: `v(h) = v_sl · sqrt(ρ0 / ρ(h))`, `ρ(h)` theo ISA.
- Khi sonde đang rơi, `v_sl` hiệu chỉnh từ tốc độ đứng đo được: `v_sl = |vv| / sqrt(ρ0 / ρ(h))`.
- Khi sonde đang lên: giả định độ cao nổ `burst_alt` và `descent_v0` trong file cấu hình.
- Trôi ngang tích phân theo lớp 100 m: `Δx = w(h) · Δh / v(h)`. `w(h)` lấy từ profile gió do chính
  sonde đo ở pha bay lên (bin 250 m). Lớp chưa quan sát dùng vận tốc ngang hiện tại.

**Phát hiện bóng nổ** (`track.c`, `flight_update`): tốc độ đứng < −3 m/s trong 3 frame liên tiếp,
độ cao > 1000 m, sau khi đã quan sát pha bay lên. Trường hợp bật máy khi sonde đã rơi: chuyển pha,
không phát cảnh báo.

**Beeper theo khoảng cách** (`track.c`): khoảng cách ngang từ máy tới sonde (hoặc tới last fix khi
mất tín hiệu).

- Bật khi d ≤ 10 km, tắt khi d > 10,5 km.
- Chu kỳ `T = 0,12 + 1,88 · (d / 10 km)^0,6` giây; mỗi tiếng 60 ms.
- Âm lượng `V = 1 − 0,35 · log10(d / 100 m)`, giới hạn 0,3–1,0.
- 2200 Hz khi telemetry đang cập nhật, 1100 Hz khi mất tín hiệu.

**Skew-T log-P** (`ui.c`): `y ∝ ln(1050 / p)` từ 1050 đến 100 hPa, isotherm nghiêng 45°,
dry adiabat `T = θ · (p/1000)^0,2857`. Dew point theo Magnus (a = 17,62; b = 243,12 °C).
Với RS41-SG (không có cảm biến áp suất), P tính từ độ cao theo ISA, hiển thị với tiền tố `~`.

**Quét phổ** (`io.c`): peak là dải bin liên tục vượt `median + scan_snr_db`; tần số tâm là trọng tâm
theo công suất, làm tròn về lưới 10 kHz.

## Bản đồ

Màn hình Map tìm mỗi ô tile theo thứ tự:

1. Gói nạp sẵn trong `tile_dir` (PNG đã đổi sang màu của UI, tạo bằng `tools/prep_tiles.py`).
2. Cache tải về trong `~/.cache/sondechase/tiles` (PNG nguyên bản của server; đổi màu khi nạp vào RAM).
3. Nếu chưa có và `tile_online = 1`: tải ô đó qua mạng bằng `curl`, tối đa 2 kết nối đồng thời. Ô tải lỗi
   không được thử lại trong 30 s.

- Chỉ các ô đang hiển thị mới được tải; app không tải trước theo vùng. Đây là kiểu sử dụng mà
  `tile.openstreetmap.org` cho phép; tải hàng loạt để dùng offline thì bị cấm.
- Mỗi request gửi User-Agent định danh ứng dụng (`tile_user_agent`). Màn hình Map hiện dòng
  "© OpenStreetMap" khi có tile.
- Cache giới hạn bởi `tile_cache_mb` (mặc định 200 MB); khi vượt, các ô cũ nhất bị xóa.
- Ô chưa có hiện lưới trống, kèm trạng thái: `Loading map`, `No network: stored map only`,
  hoặc `No map stored for this area` (khi `tile_online = 0`).
- Đổi màu (`tile_recolor` trong `src/tiles.c`): màu xuất hiện nhiều nhất của ô được coi là nền đất; độ sáng
  đầu ra tỉ lệ với khoảng cách màu so với nền, nên đường, nhãn và ranh giới thành nét sáng trên nền tối;
  pixel thiên xanh dương thành màu nước.
- `tools/prep_tiles.py` vẫn dùng được để nạp sẵn một vùng từ file MBTiles hoặc thư mục `z/x/y.png` có quyền
  dùng offline. Công cụ này không tải tile.

## Build và đóng gói

Bố cục cài đặt và gói `.deb` theo đúng hợp đồng của CardputerZero Template (`cmake/cm0-package.cmake`,
`cmake/toolchains/cp0-aarch64-linux-gnu.cmake`, tên preset giống Template):

```sh
# Cross build trên PC Debian/Ubuntu (cần crossbuild-essential-arm64, ninja-build, cmake >= 3.25).
# Lần đầu tự tải BSP sysroot của CardputerZero và LVGL v9.5.0.
cmake --workflow --preset cp0-cross-package
# -> dist/SondeChase_0.3.0_rev1_arm64.deb

# Hoặc build ngay trên máy:
cmake --workflow --preset cp0-native-package

# Cài trên CardputerZero:
sudo apt install --no-install-recommends ./SondeChase_0.3.0_rev1_arm64.deb
```

Nội dung gói:

| Đường dẫn | Nội dung |
|---|---|
| `/usr/bin/sondechase` | binary (LVGL, bộ giải mã PNG và TTF liên kết tĩnh; chỉ phụ thuộc `libc6`) |
| `/etc/sondechase.conf` | cấu hình (conffile) |
| `/usr/share/sondechase/fonts/` | Rajdhani SemiBold, Orbitron Bold |
| `/usr/share/sondechase/tiles/` | thư mục cho gói bản đồ nạp sẵn (trống) |
| `/usr/share/APPLaunch/applications/sondechase.desktop` | mục trong launcher |
| `/usr/share/APPLaunch/share/images/sondechase.png` | icon 100×100 |

`Depends: libc6, curl`. Các tiến trình con chạy lúc dùng và **không nằm trong gói**: `rtl_fm`, `rtl_power`
(rtl-sdr bản RTL-SDR Blog cho v4), `fsk_demod`, `rs41mod`, `gpsd`, `pacat`. Cài bằng:

```sh
./scripts/install_decoders.sh
```

Gói không cài systemd service: app được mở từ APPLaunch và chạy với user của launcher. User đó cần quyền
đọc/ghi `/dev/fb0`, `/dev/input/event*`, thiết bị ALSA và RTL-SDR (các group `video`, `input`, `audio`,
`plugdev`).

`app-builder.json` là manifest cho `czdev publish`; script kiểm tra của Template
(`validate_app_builder.cmake`) chấp nhận file này.

### Chế độ demo và ảnh chụp màn hình

- `demo = 1` trong file cấu hình: app phát một chuyến bay giả lập (bay lên 5 m/s tới 31,2 km, nổ, rơi dưới dù,
  chạm đất, mất tín hiệu sau 90 s) qua đúng đường xử lý của frame thật. Không cần SDR, GPS hay decoder.
  `demo_speed` là số giây mô phỏng trên một giây thực.
- `tools/render.c` chạy mã UI thật trên một display trong bộ nhớ và ghi ảnh 320×170 của từng màn hình:

```sh
cmake -S . -B build-render -DSC_RENDER=ON && cmake --build build-render -j
./build-render/sc_render <thư mục ra> assets/fonts <thư mục tile>     # ghi các file .ppm
```

Ảnh trong `screenshots/` sinh từ công cụ này. Nền bản đồ trong các ảnh Map là tile tổng hợp để kiểm tra
đường nạp PNG, không phải dữ liệu bản đồ thật.

Build trên PC để thao tác với UI: `cmake -S . -B build-sim -DSC_SIM=ON` (cần `libsdl2-dev`; chưa thử).
Chỉ build phần lõi và chạy test: `cmake -S . -B build -DSC_BUILD_UI=OFF && cmake --build build && ctest --test-dir build`.

### Nguồn tile

`prep_tiles.py` không tải tile. Đầu vào là file MBTiles raster hoặc thư mục `z/x/y.png` đã có sẵn:
tile tự render từ dữ liệu OSM, hoặc gói tile của nhà cung cấp cho phép dùng offline.
Server `tile.openstreetmap.org` không cho phép tải hàng loạt để dùng offline.
Bộ lọc `--recolor dark` chuyển tile nền sáng sang bảng màu tối của UI bằng heuristic theo khoảng cách
màu so với màu nền chiếm đa số; cần chỉnh lại nếu style tile khác nhiều so với style OSM chuẩn.

## Trạng thái kiểm thử

Đã kiểm trên host x86-64 (gcc 13, `-Wall -Wextra -Werror`):

- Phần lõi: 4165 phép kiểm trong `tests/test_core.c` đạt — geodesy, ISA, dew point, dự đoán điểm rơi,
  parser JSON rs41mod và gpsd TPV, heading, quét phổ, phát hiện bóng nổ, beeper, sinh mẫu PCM,
  lịch sử bay, toán tile, nhận diện SDR trên USB, vòng đời pipeline con (khởi động, đọc, dừng cả process group, tự khởi động lại)
  với decoder giả lập.
- `prep_tiles.py`: đọc MBTiles tổng hợp, đảo trục TMS, đổi màu.
- `ui.c`: biên dịch và liên kết với LVGL v9.5.0 thật, render headless toàn bộ 17 trạng thái màn hình với
  dữ liệu của chế độ demo; font TTF (tiny_ttf) và tile PNG (lodepng) nạp được. Bố cục đã chỉnh theo ảnh render.
- Chế độ demo: sai số dự đoán điểm rơi tại 2976 m so với điểm chạm đất mô phỏng là 79 m.
- `main.c` (nhánh framebuffer và nhánh SDL), `keys.c`: biên dịch được với header LVGL thật; chưa chạy.
- Tile: hàng đợi tải, chống trùng, đổi tên nguyên tử, lùi thời gian khi lỗi, giới hạn cache và đổi màu có unit test
  (lệnh tải được thay bằng script giả). Đường nạp tile trong UI chạy sạch dưới AddressSanitizer với cả tile
  nạp sẵn lẫn tile kiểu "tải về".
- Đóng gói: gói `.deb` thử nghiệm (kiến trúc amd64) build bằng CPack, nội dung và control đã kiểm bằng `dpkg-deb`.
- Trên CardputerZero (Debian 13): định dạng PCM của beeper phát được qua PipeWire bằng `pacat`
  (`aplay` trực tiếp bị lỗi vì PipeWire giữ codec ES8389); driver IIO `bmm150` có các node
  `in_magn_x/y/z_raw`; `rtl_test` nhận RTL-SDR Blog V4 với gói `rtl-sdr` 2.0.2 của Debian.

Chưa kiểm:

- Tốc độ vẽ trên CM0 (mỗi khung hình vẽ lại toàn màn hình; tile PNG giải mã lần đầu rồi nằm trong cache 4 MB).
- Tải tile thật từ `tile.openstreetmap.org` (môi trường build không có mạng tới server này); bộ đổi màu mới
  thử trên tile tổng hợp, chưa thử trên tile OSM thật.
- Cross build `cp0-cross` (không có toolchain aarch64 trong môi trường build) và việc cài gói arm64 trên máy.
- Pipeline `rtl_fm | fsk_demod | rs41mod` với RTL-SDR v4 và sonde thật; tham số lấy từ wiki auto_rx.
- `scripts/install_decoders.sh`.
- Framebuffer, evdev của bàn phím TCA8418, battery sysfs trên CardputerZero.
- Magnetometer: BMM150 nối phía sau BMI270; app đọc `in_magn_*_raw` qua IIO sysfs. Hướng trục cần hiệu chuẩn bằng `mag_off_x/y`, `mag_rot_deg`, `mag_flip_y`.
  Chưa có bù nghiêng.

## Giới hạn đã biết

- Chỉ hỗ trợ RS41. Loại sonde khác cần decoder và tham số `fsk_demod` riêng.
- Skew-T cắt ở 100 hPa; chưa vẽ moist adiabat và mixing ratio.
- Dự đoán điểm rơi khi sonde còn đang lên phụ thuộc `burst_alt` giả định và không có gió phía trên
  độ cao hiện tại.
- Lịch sử bay giữ 2048 mẫu (5 s/mẫu, tự giảm độ phân giải khi đầy); không khôi phục sau khi khởi động lại app.
  Log CSV đầy đủ ghi tại `~/sondechase.csv`.

## Giấy phép

SondeChase phát hành theo GNU General Public License phiên bản 3 hoặc (tùy bạn chọn) bất kỳ phiên bản nào mới hơn (`GPL-3.0-or-later`), xem [`LICENSE`](LICENSE).
Giấy phép này cho phép đưa kèm các decoder upstream vào repo hoặc gói cài đặt.

Thành phần bên thứ ba:

| Thành phần | Giấy phép |
|---|---|
| `rs41mod` (zilog80, [rs1729/RS](https://github.com/rs1729/RS), qua [radiosonde_auto_rx](https://github.com/projecthorus/radiosonde_auto_rx)) | GPL-3.0 |
| `fsk_demod` ([codec2](https://github.com/drowe67/codec2), David Rowe) | LGPL-2.1 |
| [LVGL](https://github.com/lvgl/lvgl) v9.5 | MIT |
| Các file trong `cmake/` lấy từ CardputerZero Template | MIT, xem `cmake/LICENSE-CardputerZero-Template.txt` |
| Font Rajdhani, Orbitron | SIL OFL 1.1, xem `assets/fonts/OFL-*.txt` |
| Dữ liệu bản đồ | © OpenStreetMap contributors, ODbL |
