# Resogun PC 3D

3D for the [Resogun PC port](https://github.com/rcostanza/resogun-recomp): both eyes are drawn from the same frame (Vulkan multiview), woven for an **SR panel** (Acer SpatialLabs and other Leia SR displays) or shown **side by side** for a 3D TV / 3D monitor.

**Download:** [Releases](https://github.com/nautymac/resogun-recomp/releases) — `resogun-3d-<version>-windows.zip`, used together with the original release it names.

[English](#english) · [한국어](#한국어)

---

## English

### Install — 3 steps

1. **Download two zips:** the original Resogun PC release the 3D zip is made for (v0.10: [resogun-pc-v0.10-2026-11-11.zip](https://github.com/rcostanza/resogun-recomp/releases/download/v0.10/resogun-pc-v0.10-2026-11-11.zip)) and the 3D zip.
2. **Unpack both to the same place** — the original first, then the 3D zip (replace files). Both have the same folder inside, so they end up in one folder. For a folder you already use, copy the files *inside* the 3D zip's folder into it.
3. **Run `resogun-setup.exe`** → *Game folder*: your PS4 dump (CUSA00038, v1.10) → *3D (stereo)*: **SR panel** or **Side by side** → **Install** → **Play Game**.

### Use

| To do | How |
|---|---|
| Play | `resogun.exe` or the desktop shortcut — it starts in the 3D mode chosen in the setup |
| Change the mode, or back to 2D | `resogun-setup.exe` → *3D (stereo)*: Off / SR panel / Side by side — saved at once, the next start uses it |
| Game settings (window, FPS, VSync, resolution, HDR, speakers, 3D depth) | `resogun-setup.exe` → **Game settings** — each choice is saved at once; **Back** returns |
| 3D on/off while playing | **Ctrl+F3** (between the setup's mode and off) |
| Less / more depth | **Ctrl+F5 / Ctrl+F6** (0.1 per press) |
| Scene forward / back | **Ctrl+F7 / Ctrl+F8** (convergence ÷1.1 / ×1.1) |
| Swap the eyes | **Ctrl+F9** |
| Depth and convergence on the gamepad | Hold **L1 + R1** and press the D-pad: **← / →** less / more depth, **↑ / ↓** scene back / forward. Held, it repeats |

Each key shows the new values at the top of the picture and saves them at once (L1 + R1: when you let go of them). While L1 + R1 are held the game gets neither of them nor the D-pad; a single L1 or R1 reaches the game 0.06 s late, the time to press the other one.

**Getting it comfortable:** start with the defaults (depth 1, convergence 60). At 60 the ship and the HUD sit at the screen's depth and the city is behind it. If your eyes strain or things look doubled, lower the depth (Ctrl+F5, or L1 + R1 + ←). Lower convergence (Ctrl+F7, or L1 + R1 + ↓) brings the whole scene out of the screen; higher (Ctrl+F8, or L1 + R1 + ↑) pushes it in. If near things go in and far things come out, swap the eyes (Ctrl+F9).

### Settings (settings.ini)

The game reads `settings.ini` in the save folder (else the one next to `resogun.exe`). The setup (its 3D row and its **Game settings** page) and the keys write these lines:

| Key | Values | Default |
|---|---|---|
| `stereo` | `off`, `sr` (SR panel), `sbs` (side by side) | `off` |
| `stereo_depth` | % of the picture's width far things move per eye (0–10) | `1` |
| `stereo_convergence` | the distance shown at the screen's depth, game units | `60` |
| `stereo_swap` | `1` swaps the eyes | `0` |

While 3D is on the game renders at 1080p in 16:9 (`render_resolution=4k` and `aspect_ratio` apply in 2D). An SR panel gets 1920x2160 per eye.

### DLC (ships and skins, Heroes, Defenders)

Resogun's add-ons are entitlement-only: each DLC package holds only `sce_sys` (param.sfo and an icon), no game data. The game sees an add-on as installed when a folder named after its 16-character label exists:

```
CUSA00038-DLC\
  RESOGUNHEROES002\sce_sys\param.sfo, icon0.png
  RESOGUNDEFEND002\sce_sys\...
  ASTRONAUT0000002\sce_sys\...
  ...
```

The label is the end of the package's content ID (`UP9000-CUSA00038_00-RESOGUNHEROES002` → `RESOGUNHEROES002`). Put the folders in a folder of their own and choose it as **Extra DLC folder** in `resogun-setup.exe` (then **Install**), or put them in `addcont\` inside the save folder. shadPS4's `addcont` folder works as it is. `save\log\resogun.log` lists each one as `Runtime: add-on <label> installed`.

### Troubleshooting

- **SR panel chosen but the picture is side by side:** the SR runtime is not installed or not running, or `ps2sr.dll` is missing next to `resogun.exe` (the setup's 3D row says so).
- **Stutter at the first start or in a new scene:** shaders are being built; it goes away.
- **What happened:** `save\log\resogun.log` — the lines starting with `Stereo 3D:` say whether 3D started and whether the SR weaver is weaving.
- **Back to the original:** unpack the original zip over the folder again and choose 3D **Off** in the setup.

Source: this branch (`stereo-3d`), GPL-2.0-or-later like the original.

---

## 한국어

### 설치 — 3단계

1. **zip 두 개 받기:** 3D zip 이 기준으로 삼는 원작 Resogun PC 릴리스(v0.10: [resogun-pc-v0.10-2026-11-11.zip](https://github.com/rcostanza/resogun-recomp/releases/download/v0.10/resogun-pc-v0.10-2026-11-11.zip))와 3D zip.
2. **같은 곳에 둘 다 풀기** — 원작 먼저, 3D zip 나중에(덮어쓰기). 두 zip 안 폴더 이름이 같아서 한 폴더로 합쳐집니다. 이미 쓰고 있는 폴더라면 3D zip 안 폴더의 *내용*을 그 폴더에 덮어 복사합니다.
3. **`resogun-setup.exe` 실행** → *Game folder*: PS4 덤프 (CUSA00038, 버전 1.10) → *3D (stereo)*: **SR panel** 또는 **Side by side** → **Install** → **Play Game**.

### 사용법

| 하려는 것 | 방법 |
|---|---|
| 게임 시작 | `resogun.exe` 또는 바탕화면 바로가기 — 셋업에서 고른 3D 방식으로 시작 |
| 방식 바꾸기, 2D로 돌아가기 | `resogun-setup.exe` → *3D (stereo)*: Off / SR panel / Side by side — 고르면 바로 저장, 다음 실행부터 적용 |
| 게임 설정 (창 모드, FPS, VSync, 해상도, HDR, 스피커, 3D 깊이) | `resogun-setup.exe` → **Game settings** — 고르면 바로 저장, **Back** 으로 돌아감 |
| 게임 중 3D 켜기/끄기 | **Ctrl+F3** (셋업에서 고른 방식 ↔ 끔) |
| 깊이 줄이기 / 늘리기 | **Ctrl+F5 / Ctrl+F6** (한 번에 0.1) |
| 장면을 앞으로 / 뒤로 | **Ctrl+F7 / Ctrl+F8** (수렴 ÷1.1 / ×1.1) |
| 왼쪽·오른쪽 눈 바꾸기 | **Ctrl+F9** |
| 게임패드로 깊이·수렴 조절 | **L1 + R1** 을 누른 채 십자키: **← / →** 깊이 줄이기 / 늘리기, **↑ / ↓** 장면을 뒤로 / 앞으로. 누르고 있으면 반복 |

키를 누르면 바뀐 값이 화면 위에 잠깐 뜨고 바로 저장됩니다(L1 + R1 은 손을 뗄 때 저장). L1 + R1 을 누르고 있는 동안 게임에는 두 버튼과 십자키가 가지 않습니다. L1 이나 R1 하나만 누르면 다른 쪽을 누를 시간(0.06초)만큼 늦게 게임에 갑니다.

**편하게 맞추기:** 기본값(깊이 1, 수렴 60)으로 시작합니다. 수렴 60 이면 기체와 HUD 가 화면 깊이에 오고 도시는 화면 뒤에 있습니다. 눈이 피곤하거나 겹쳐 보이면 깊이를 줄입니다(Ctrl+F5, 또는 L1 + R1 + ←). 수렴을 낮추면(Ctrl+F7, 또는 L1 + R1 + ↓) 장면 전체가 화면 밖으로 나오고, 높이면(Ctrl+F8, 또는 L1 + R1 + ↑) 안으로 들어갑니다. 가까운 것이 들어가고 먼 것이 나와 보이면 눈을 바꿉니다(Ctrl+F9).

### 설정 (settings.ini)

게임은 save 폴더의 `settings.ini` 를 읽습니다(없으면 `resogun.exe` 옆의 것). 셋업(3D 줄과 **Game settings** 페이지)과 단축키가 아래 줄을 씁니다.

| 키 | 값 | 기본값 |
|---|---|---|
| `stereo` | `off`, `sr` (SR 패널), `sbs` (좌우) | `off` |
| `stereo_depth` | 먼 물체가 눈마다 움직이는 거리, 화면 폭의 % (0–10) | `1` |
| `stereo_convergence` | 화면 깊이에 보이는 거리, 게임 단위 | `60` |
| `stereo_swap` | `1` 이면 눈을 바꿈 | `0` |

3D 동안은 1080p·16:9 로 그립니다(`render_resolution=4k` 와 `aspect_ratio` 는 2D 에서 쓰입니다). SR 패널에는 눈마다 1920x2160 으로 나갑니다.

### DLC (기체·스킨, Heroes, Defenders)

Resogun 애드온은 권한 표시용입니다. DLC 패키지마다 `sce_sys`(param.sfo 와 아이콘)만 있고 게임 데이터는 없습니다. 게임은 16자 라벨 이름의 폴더가 있으면 그 애드온이 설치된 것으로 봅니다.

```
CUSA00038-DLC\
  RESOGUNHEROES002\sce_sys\param.sfo, icon0.png
  RESOGUNDEFEND002\sce_sys\...
  ASTRONAUT0000002\sce_sys\...
  ...
```

라벨은 패키지 콘텐츠 ID 의 끝부분입니다(`UP9000-CUSA00038_00-RESOGUNHEROES002` → `RESOGUNHEROES002`). 이 폴더들을 한 폴더에 모아 `resogun-setup.exe` 의 **Extra DLC folder** 로 고르고 **Install** 을 누르거나, save 폴더 안 `addcont\` 에 넣습니다. shadPS4 의 `addcont` 폴더도 그대로 쓸 수 있습니다. `save\log\resogun.log` 에 애드온마다 `Runtime: add-on <라벨> installed` 줄이 나옵니다.

### 문제 해결

- **SR panel 을 골랐는데 좌우로 나옴:** SR 런타임이 설치·실행되어 있지 않거나, `resogun.exe` 옆에 `ps2sr.dll` 이 없습니다(셋업의 3D 줄에 표시됩니다).
- **처음 실행·새 장면에서 끊김:** 셰이더를 만드는 중이라 곧 없어집니다.
- **무슨 일이 있었는지 볼 때:** `save\log\resogun.log` 에서 `Stereo 3D:` 로 시작하는 줄이 3D 시작과 SR 위빙 여부를 알려 줍니다.
- **원래대로:** 원작 zip 을 다시 덮어 풀고 셋업에서 3D 를 **Off** 로 고릅니다.

소스: 이 브랜치(`stereo-3d`), 원작과 같은 GPL-2.0-or-later.
