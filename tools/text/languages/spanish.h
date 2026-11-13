/* -*- coding: windows-1252-unix; -*- */
/*
 * Copyright (C) 2019-2024 Roger Clark, VK3KYY / G4KYF
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*
 * Translators:
 *
 *
 * Rev:
 */
#ifndef USER_INTERFACE_LANGUAGES_SPANISH_H_
#define USER_INTERFACE_LANGUAGES_SPANISH_H_
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with windows-1252-unix encoding
 *
 ********************************************************************/
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
__attribute__((section(".upper_text")))
#endif
const stringsTable_t spanishLanguage =
{
.magicNumber                            = { LANGUAGE_TAG_MAGIC_NUMBER, LANGUAGE_TAG_VERSION },
.LANGUAGE_NAME 				= "Espanol", // MaxLen: 16
.menu					= "Menu", // MaxLen: 16
.credits				= "Creditos", // MaxLen: 16
.zone					= "Zona", // MaxLen: 16
.rssi					= "RSSI", // MaxLen: 16
.battery				= "Bateria", // MaxLen: 16
.contacts				= "Contactos", // MaxLen: 16
.last_heard				= "Ult. escuchado", // MaxLen: 16
.firmware_info				= "Info firmware", // MaxLen: 16
.options				= "Opciones", // MaxLen: 16
.display_options			= "Opc. pantalla", // MaxLen: 16
.sound_options				= "Opc. sonido", // MaxLen: 16
.channel_details			= "Detalle canal", // MaxLen: 16
.language				= "Idioma", // MaxLen: 16
.new_contact				= "Nuevo contacto", // MaxLen: 16
.dmr_contacts				= "Contactos DMR", // MaxLen: 16
.contact_details			= "Detalle contacto", // MaxLen: 16
.hotspot_mode				= "Modo Hotspot", // MaxLen: 16
.built					= "Compilado", // MaxLen: 16
.zones					= "Zonas", // MaxLen: 16
.keypad					= "Teclado", // MaxLen: 12 (with .ptt)
.ptt					= "PTT", // MaxLen: 12 (with .keypad, .mode)
.locked					= "Bloqueado", // MaxLen: 15
.press_sk2_plus_star			= "Pulsar SK2 + *", // MaxLen: 16
.to_unlock				= "para desbloquear", // MaxLen: 16
.unlocked				= "Desbloqueado", // MaxLen: 15
.power_off				= "Apagando...", // MaxLen: 16
.error					= "ERROR", // MaxLen: 8
.rx_only				= "Solo RX", // MaxLen: 14
.out_of_band				= "FUERA DE BANDA", // MaxLen: 14
.timeout				= "TOT", // MaxLen: 8
.tg_entry				= "TG entrada", // MaxLen: 15
.pc_entry				= "PC entrada", // MaxLen: 15
.user_dmr_id				= "ID DMR usuario", // MaxLen: 15
.contact 				= "Contacto", // MaxLen: 15
.accept_call				= "Devolver llamada", // MaxLen: 16
.private_call				= "Llamada privada", // MaxLen: 16
.squelch				= "Squelch", // MaxLen: 8
.quick_menu 				= "Menu rapido", // MaxLen: 16
.filter					= "Filtro", // MaxLen: 7 (with ':' + settings: .none, "CC", "CC,TS", "CC,TS,TG")
.all_channels				= "Todos canales", // MaxLen: 16
.gotoChannel				= "IR A",  // MaxLen: 11 (" 1024")
.scan					= "Scan", // MaxLen: 16
.channelToVfo				= "Canal --> VFO", // MaxLen: 16
.vfoToChannel				= "VFO --> Canal", // MaxLen: 16
.vfoToNewChannel			= "VFO --> Nuevo", // MaxLen: 16
.group					= "Grupo", // MaxLen: 16 (with .type)
.private				= "Privado", // MaxLen: 16 (with .type)
.all					= "Todos", // MaxLen: 16 (with .type)
.type					= "Tipo", // MaxLen: 16 (with .type)
.timeSlot				= "Ranura", // MaxLen: 16 (plus ':' and  .none, '1' or '2')
.none					= "Ninguno", // MaxLen: 16 (with .timeSlot, "Rx CTCSS:" and ""Tx CTCSS:", .filter/.mode/.dmr_beep)
.contact_saved				= "Guardado", // MaxLen: 16
.duplicate				= "Duplicado", // MaxLen: 16
.tg					= "TG",  // MaxLen: 8
.pc					= "PC", // MaxLen: 8
.ts					= "TS", // MaxLen: 8
.mode					= "Modo",  // MaxLen: 12
.colour_code				= "Codigo color", // MaxLen: 16 (with ':' * .n_a)
.n_a					= "N/A",// MaxLen: 16 (with ':' * .colour_code)
.bandwidth				= "Ancho banda", // MaxLen: 16 (with : + .n_a, "25kHz" or "12.5kHz")
.stepFreq				= "Paso", // MaxLen: 7 (with ':' + xx.xxkHz fitted)
.tot					= "TOT", // MaxLen: 16 (with ':' + .off or 15..3825)
.off					= "No", // MaxLen: 16 (with ':' + .timeout_beep, .band_limits, aprs_decay)
.zone_skip				= "Salto zona", // MaxLen: 16 (with ':' + .yes or .no)
.all_skip				= "Saltar todo", // MaxLen: 16 (with ':' + .yes or .no)
.yes					= "Si", // MaxLen: 16 (with ':' + .zone_skip, .all_skip)
.no					= "No", // MaxLen: 16 (with ':' + .zone_skip, .all_skip, .aprs_decay)
.tg_list				= "Lista TG", // MaxLen: 16 (with ':' and codeplug group name)
.on					= "Si", // MaxLen: 16 (with ':' + .band_limits)
.timeout_beep				= "Aviso timeout", // MaxLen: 16 (with ':' + .n_a or 5..20 + 's')
.list_full				= "Lista llena",
.dmr_cc_scan				= "Scan CC", // MaxLen: 12 (with ':' + settings: .on or .off).
.band_limits				= "Lim. banda", // MaxLen: 16 (with ':' + .on or .off)
.beep_volume				= "Vol. aviso", // MaxLen: 16 (with ':' + -24..6 + 'dB')
.dmr_mic_gain				= "Mic DMR", // MaxLen: 16 (with ':' + -33..12 + 'dB')
.fm_mic_gain				= "Mic FM", // MaxLen: 16 (with ':' + 0..31)
.key_long				= "Tecla larga", // MaxLen: 11 (with ':' + x.xs fitted)
.key_repeat				= "Rep. tecla", // MaxLen: 11 (with ':' + x.xs fitted)
.dmr_filter_timeout			= "Filtro tiempo", // MaxLen: 16 (with ':' + 1..90 + 's')
.brightness				= "Brillo", // MaxLen: 16 (with ':' + 0..100 + '%')
.brightness_off				= "Brillo min", // MaxLen: 16 (with ':' + 0..100 + '%')
.contrast				= "Contraste", // MaxLen: 16 (with ':' + 12..30)
.screen_invert				= "Invertido", // MaxLen: 16
.screen_normal				= "Normal", // MaxLen: 16
.backlight_timeout			= "Tiempo apagado", // MaxLen: 16 (with ':' + .no to 30s)
.scan_delay				= "Demora scan", // MaxLen: 16 (with ':' + 1..30 + 's')
.yes___in_uppercase			= "SI", // MaxLen: 8 (choice above green/red buttons)
.no___in_uppercase			= "NO", // MaxLen: 8 (choice above green/red buttons)
.DISMISS				= "CERRAR", // MaxLen: 8 (choice above green/red buttons)
.scan_mode				= "Modo scan", // MaxLen: 16 (with ':' + .hold, .pause or .stop)
.hold					= "Mantener", // MaxLen: 16 (with ':' + .scan_mode)
.pause					= "Pausa", // MaxLen: 16 (with ':' + .scan_mode)
.list_empty				= "Lista vacia", // MaxLen: 16
.delete_contact_qm			= "Eliminar?", // MaxLen: 16
.contact_deleted			= "Eliminado", // MaxLen: 16
.contact_used				= "Contacto usado", // MaxLen: 16
.in_tg_list				= "en lista TG", // MaxLen: 16
.select_tx				= "Selecc. TX", // MaxLen: 16
.edit_contact				= "Editar contacto", // MaxLen: 16
.delete_contact				= "Eliminar", // MaxLen: 16
.group_call				= "Llamada grupo", // MaxLen: 16
.all_call				= "Llamar a todos", // MaxLen: 16
.tone_scan				= "Scan tono", // MaxLen: 16
.low_battery				= "BATERIA BAJA!", // MaxLen: 16
.Auto					= "Auto", // MaxLen 16 (with .mode + ':', .mode)
.manual					= "Manual",  // MaxLen 16 (with .mode + ':', .mode)
.ptt_toggle				= "PTT fijo", // MaxLen 16 (with ':' + .on or .off)
.private_call_handling			= "Permitir PC", // MaxLen 16 (with ':' + .on or .off)
.stop					= "Parar", // Maxlen 16 (with ':' + .scan_mode/.dmr_beep)
.one_line				= "1 linea", // MaxLen 16 (with ':' + .contact)
.two_lines				= "2 lineas", // MaxLen 16 (with ':' + .contact)
.new_channel				= "Nuevo canal", // MaxLen: 16, leave room for a space and four channel digits after
.priority_order				= "Orden", // MaxLen 16 (with ':' + 'Cc/DB/TA')
.dmr_beep				= "Aviso DMR", // MaxLen 16 (with ':' + .star/.stop/.both/.none)
.start					= "Inicio", // MaxLen 16 (with ':' + .dmr_beep)
.both					= "Ambos", // MaxLen 16 (with ':' + .dmr_beep)
.vox_threshold                          = "Umbral VOX", // MaxLen 16 (with ':' + .off or 1..30)
.vox_tail                               = "Cola VOX", // MaxLen 16 (with ':' + .n_a or '0.0s')
.audio_prompt				= "Aviso audio",// Maxlen 16 (with ':' + .silent, .beep or .voice_prompt_level_1)
.silent                                 = "Silencio", // Maxlen 16 (with : + audio_prompt)
.rx_beep				= "Aviso RX", // MaxLen 16 (with ':' + .carrier/.talker/.both/.none)
.beep					= "Aviso", // Maxlen 16 (with : + audio_prompt)
.voice_prompt_level_1			= "Voces", // Maxlen 16 (with : + audio_prompt, satellite "mode")
.transmitTalkerAliasTS1			= "TA Tx TS1", // Maxlen 16 (with : + .on or .off)
.squelch_VHF				= "Squelch VHF",// Maxlen 16 (with : + XX%)
.squelch_220				= "Squelch 220",// Maxlen 16 (with : + XX%)
.squelch_UHF				= "Squelch UHF", // Maxlen 16 (with : + XX%)
.display_screen_invert 			= "Pant. invertida", // Maxlen 16 (with : + .screen_normal or .screen_invert)
.openGD77 				= "OpenGD77",// Do not translate
.talkaround 				= "Talkaround", // Maxlen 16 (with ':' + .on , .off or .n_a)
.APRS 					= "APRS", // Maxlen 16 (with : + .transmitTalkerAliasTS1 or transmitTalkerAliasTS2)
.no_keys 				= "Sin teclas", // Maxlen 16 (with : + audio_prompt)
.gitCommit				= "Commit Git",
.voice_prompt_level_2			= "Voces L2", // Maxlen 16 (with : + audio_prompt)
.voice_prompt_level_3			= "Voces L3", // Maxlen 16 (with : + audio_prompt)
.dmr_filter				= "Filtro DMR",// MaxLen: 12 (with ':' + settings: "TG" or "Ct" or "TGL")
.talker					= "Locutor",
.dmr_ts_filter				= "Filtro TS", // MaxLen: 12 (with ':' + settings: .on or .off)
.dtmf_contact_list			= "Contactos DTMF", // Maxlen: 16
.channel_power				= "Pot. canal", //Displayed as "Ch Power:" + .from_master or "Ch Power:"+ power text e.g. "Power:500mW" . Max total length 16
.from_master				= "Maestro",// Displayed if per-channel power is not enabled  the .channel_power, and also with .location in APRS settings.
.set_quickkey				= "Guardar rapido", // MaxLen: 16
.dual_watch				= "Doble escucha", // MaxLen: 16
.info					= "Info", // MaxLen: 16 (with ':' + .off or .ts or .pwr or .both)
.pwr					= "Pot.",
.user_power				= "Pot. usuario",
.temperature				= "Temperatura", // MaxLen: 16 (with ':' + .celcius or .fahrenheit)
.celcius				= "°C",
.seconds				= "segundos",
.radio_info				= "Info radio",
.temperature_calibration		= "Calib. temp.",
.pin_code				= "Codigo PIN",
.please_confirm				= "Confirmar?", // MaxLen: 15
.vfo_freq_bind_mode			= "Vinc. frec.",
.overwrite_qm				= "Sobrescribir?", //Maxlen: 14 chars
.eco_level				= "Nivel eco",
.buttons				= "Botones",
.leds					= "LEDs",
.scan_dwell_time			= "Tiempo scan",
.battery_calibration			= "Calib. bat.",
.low					= "Bajo",
.high					= "Alto",
.dmr_id					= "ID DMR",
.scan_on_boot				= "Scan al inicio",
.dtmf_entry				= "Entrada DTMF",
.name					= "Nombre",
.carrier				= "Portadora",
.zone_empty 				= "Zona vacia", // Maxlen: 12 chars.
.time					= "Hora",
.uptime					= "Activo",
.hours					= "Horas",
.minutes				= "Minutos",
.satellite				= "Satelite",
.alarm_time				= "Hora alarma",
.location				= "Ubicacion",
.date					= "Fecha",
.timeZone				= "Zona horaria",
.suspend				= "Suspender",
.pass					= "Pase", // For satellite screen
.elevation				= "El",
.azimuth				= "Az",
.inHHMMSS				= "en",
.predicting				= "Prediciendo",
.maximum				= "Max",
.satellite_short		= "Sat",
.local					= "Local",
.UTC					= "UTC",
.symbols				= "NSEO", // symbols: N,S,E,W
.not_set				= "SIN AJUSTE",
.general_options		= "Opc. generales",
.radio_options			= "Opc. radio",
.auto_night				= "Noche auto", // MaxLen: 16 (with .on or .off)
.dmr_rx_agc				= "AGC RX DMR",
.speaker_click_suppress			= "Supr. clic",
.gps					= "GPS",
.end_only				= "Solo fin",
.dmr_crc				= "CRC DMR",
.eco					= "Eco",
.safe_power_on				= "Encendido seguro", // MaxLen: 16 (with ':' + .on or .off)
.auto_power_off				= "Auto apagado", // MaxLen: 16 (with ':' + 30/60/90/120/180 or .no)
.apo_with_rf				= "APO con RF", // MaxLen: 16 (with ':' + .yes or .no or .n_a)
.brightness_night				= "Brillo noche", // MaxLen: 16 (with : + 0..100 + %)
.freq_set_VHF			= "Frec. VHF",
.gps_acquiring			= "Buscando",
.altitude				= "Alt",
.calibration            = "Calibracion",
.freq_set_UHF           = "Frec. UHF",
.cal_frequency          = "Frec.",
.cal_pwr                = "Nivel pot.",
.pwr_set                = "Ajustar",
.factory_reset          = "Restabl. fabrica",
.rx_tune				= "Ajuste RX",
.transmitTalkerAliasTS2	= "TA Tx TS2", // Maxlen 16 (with : + .ta_text, 'APRS' , .both or .off)
.ta_text				= "Texto",
.daytime_theme_day			= "Tema dia", // MaxLen: 16
.daytime_theme_night			= "Tema noche", // MaxLen: 16
.theme_chooser				= "Elegir tema", // Maxlen: 16
.theme_options				= "Opc. tema",
.theme_fg_default			= "Texto def.", // MaxLen: 16 (+ colour rect)
.theme_bg				= "Fondo", // MaxLen: 16 (+ colour rect)
.theme_fg_decoration			= "Decoracion", // MaxLen: 16 (+ colour rect)
.theme_fg_text_input			= "Entrada texto", // MaxLen: 16 (+ colour rect)
.theme_fg_splashscreen			= "Texto arranque", // MaxLen: 16 (+ colour rect)
.theme_bg_splashscreen			= "Fondo arranque", // MaxLen: 16 (+ colour rect)
.theme_fg_notification			= "Texto aviso", // MaxLen: 16 (+ colour rect)
.theme_fg_warning_notification		= "Aviso precaucion", // MaxLen: 16 (+ colour rect)
.theme_fg_error_notification		= "Texto error", // MaxLen: 16 (+ colour rect)
.theme_bg_notification                  = "Fondo aviso", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_name			= "Nombre menu", // MaxLen: 16 (+ colour rect)
.theme_bg_menu_name			= "Fondo menu", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_item			= "Item menu", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_item_selected		= "Item resaltado", // MaxLen: 16 (+ colour rect)
.theme_fg_options_value			= "Valor opcion", // MaxLen: 16 (+ colour rect)
.theme_fg_header_text			= "Texto cabecera", // MaxLen: 16 (+ colour rect)
.theme_bg_header_text			= "Fondo cabecera", // MaxLen: 16 (+ colour rect)
.theme_fg_rssi_bar			= "Barra RSSI", // MaxLen: 16 (+ colour rect)
.theme_fg_rssi_bar_s9p			= "Barra RSSI S9+", // Maxlen: 16 (+colour rect)
.theme_fg_channel_name			= "Nombre canal", // MaxLen: 16 (+ colour rect)
.theme_fg_channel_contact		= "Contacto", // MaxLen: 16 (+ colour rect)
.theme_fg_channel_contact_info		= "Info contacto", // MaxLen: 16 (+ colour rect)
.theme_fg_zone_name			= "Nombre zona", // MaxLen: 16 (+ colour rect)
.theme_fg_rx_freq			= "Frec. RX", // MaxLen: 16 (+ colour rect)
.theme_fg_tx_freq			= "Frec. TX", // MaxLen: 16 (+ colour rect)
.theme_fg_css_sql_values		= "Valores CSS/SQL", // MaxLen: 16 (+ colour rect)
.theme_fg_tx_counter			= "Contador TX", // MaxLen: 16 (+ colour rect)
.theme_fg_polar_drawing			= "Polar", // MaxLen: 16 (+ colour rect)
.theme_fg_satellite_colour		= "Punto sat.", // MaxLen: 16 (+ colour rect)
.theme_fg_gps_number			= "Num. GPS", // MaxLen: 16 (+ colour rect)
.theme_fg_gps_colour			= "Punto GPS", // MaxLen: 16 (+ colour rect)
.theme_fg_bd_colour			= "Punto BeiDou", // MaxLen: 16 (+ colour rect)
.theme_colour_picker_red		= "Rojo", // MaxLen 16 (with ':' + 3 digits value)
.theme_colour_picker_green		= "Verde", // MaxLen 16 (with ':' + 3 digits value)
.theme_colour_picker_blue		= "Azul", // MaxLen 16 (with ':' + 3 digits value)
.volume					= "Volumen", // MaxLen: 8
.distance_sort				= "Orden dist.", // MaxLen 16 (with ':' + .on or .off)
.show_distance				= "Ver dist.", // MaxLen 16 (with ':' + .on or .off)
.aprs_options				= "Opc. APRS", // MaxLen 16
.aprs_smart				= "Inteligente", // MaxLen 16 (with ':' + .mode)
.aprs_channel				= "Canal", // MaxLen 16 (with ':' + .location)
.aprs_decay				= "Decaim.", // MaxLen 16 (with ':' + .on or .off)
.aprs_compress				= "Comprimir", // MaxLen 16 (with ':' + .on or .off)
.aprs_interval				= "Intervalo", // MaxLen 16 (with ':' + 0.2..60 + 'min')
.aprs_message_interval			= "Int. mensaje", // MaxLen 16 (with ':' + 3..30)
.aprs_slow_rate				= "Tasa lenta", // MaxLen 16 (with ':' + 1..100 + 'min')
.aprs_fast_rate				= "Tasa rapida", // MaxLen 16 (with ':' + 10..180 + 's')
.aprs_low_speed				= "Vel. baja", // MaxLen 16 (with ':' + 2..30 + 'km/h')
.aprs_high_speed			= "Vel. alta", // MaxLen 16 (with ':' + 2..90 + 'km/h')
.aprs_turn_angle			= "Ang. giro", // MaxLen 16 (with ':' + 5..90 + '°')
.aprs_turn_slope			= "Pend. giro", // MaxLen 16 (with ':' + 1..255 + '°/v')
.aprs_turn_time				= "Tiempo giro", // MaxLen 16 (with ':' + 5..180 + 's')
.auto_lock				= "Bloq. auto", // MaxLen 16 (with ':' + .off or 0.5..15 (.5 step) + 'min')
.trackball				= "Trackball", // MaxLen 16 (with ':' + .on or .off)
.dmr_force_dmo				= "Forzar DMO", // MaxLen 16 (with ':' + .n_a or .on or .off)
.boot_screen				= "Pant. inicio", // MaxLen 16 (with ':' + .on or .off)
.ui_mic = "Mic",
.ui_waking_repeater = "Despertar rep.",
.ui_no_repeater = "Sin repetidora",
.ui_time_left_of = "Quedan",
.ui_elapsed = "Transcurrido",
.ui_offset = "Desfase",
.ui_select = "Elegir",
.ui_back = "Volver",
.ui_delete = "Borrar",
.ui_sk2_knob = "SK2+Perilla",
.fm_radio = "Radio FM",
.ui_stereo = "Estereo",
.ui_seeking = "Buscando",
.ui_fm_hint = "Giro: sinton. #*: ir",
.ui_latitude = "Lat",
.ui_longitude = "Lon",
.ui_no_fix = "Sin fix",
.ui_sats = "Sats",
.ui_in_pass = "En pase",
.ui_time_left = "Restante",
.ui_time_to = "En",
.ui_rx = "RX",
.ui_tx = "TX",
.ui_tone = "Tono",
.ui_no_keps = "Cargar keps (CPS)",
.ui_start = "Inicio",
.ui_duration = "Duracion",
.ui_dw_add = "Anadir a DW",
.ui_dw_tx = "TX doble esc.",
.ui_dw_follow = "Seguir",
.ui_dw_watching = "Vigilando",
.ui_dw_receiving = "Recibiendo",
.ui_dw_replace = "Reemplazar",
.ui_me = "yo",
.ui_minutes_short = "min",
.ui_hours_short = "h",
.ui_number = "Numero",
.ui_save = "Guardar",
.ui_tx_refused = "No se puede TX",
.ui_tx_timeout = "Timeout TX",
.ui_rx_only_hint = "Fijar Solo RX en CPS",
.ui_out_of_band_hint = "No es banda TX",
.ui_release_ptt = "Soltar el PTT",
.ui_unlock_hint = "SK2 + * desbloq.",
.ui_pc_incoming = "Llam. entrante",
.ui_call_back_qm = "Devolver?",
.ui_pc_to = "PC a",
.ui_hash_type = "# Tipo",
.ui_up_down_step = "Arr./Abajo: +/-1",
.ui_now = "Ahora",
.ui_ok_save_id = "OK: guardar",
.ui_select_contact = "Elegir contacto",
.ui_cancel = "Cancelar",
.ui_qk_open = "OK: abrir",
.ui_qk_knob = "Perilla > + < -",
.ui_chip = "Chip",
.ui_model = "Modelo",
.ui_battery_graph = "Grafico bateria",
.ui_up_down = "Arr./Abajo",
.ui_down_full_list = "Abajo: completa",
.ui_edit = "Editar",
};
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with windows-1252-unix encoding
 *
 ********************************************************************/
#endif /* USER_INTERFACE_LANGUAGES_SPANISH_H_ */
