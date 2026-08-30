/**
 * @file    test_gps.c
 * @brief   gps_daemon 模块单元测试 — NMEA 解析 & 地理计算
 *
 * 测试内容:
 *   1. NMEA 度分 → 十进制度转换
 *   2. Haversine 距离计算 (已知两点验证)
 *   3. NMEA GGA 语句: 字符级状态机解析 + 数据提取
 *   4. NMEA RMC 语句: 速度/日期提取
 *   5. 校验和检测: 正确/错误
 *   6. 字段不完整/格式容错
 *   7. NMEA GSV 语句: 卫星数提取
 *   8. NMEA GLL 语句: 地理定位信息 (经纬度/时间/状态)
 *   9. NMEA VTG 语句: 地面速度信息 (km/h直接+节回退)
 *  10. NMEA GSA 语句: 定位类型(2D/3D) + HDOP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "gps_daemon.h"          /* src/gps_daemon/ */
#include "nmea_parser.h"
#include "log/log.h"             /* include/log/    */

static int g_passed = 0, g_failed = 0, g_total = 0;

#define TEST_START(n) printf("\n========== [TEST] %s ==========\n", n)
#define TEST_CASE(d)  do { g_total++; printf("  [%d] %s ... ", g_total, d); } while(0)
#define TEST_OK()     do { printf("OK\n"); g_passed++; } while(0)
#define TEST_FAIL(m)  do { printf("FAIL: %s\n", m); g_failed++; return; } while(0)
#define TEST_ASSERT(c,m) do { if(!(c)) TEST_FAIL(m); } while(0)

/* ---- 辅助: 逐字符喂入 NMEA 语句 ---- */
static int feed_sentence(nmea_parser_t *p, const char *s)
{
    int ret = 0;
    for (; *s; s++) {
        int r = nmea_parser_feed(p, *s);
        if (r != 0) ret = r;
    }
    return ret;
}

/* ---- 测试 1: 度分转换 ---- */
static void test_degrees_conversion(void)
{
    TEST_START("NMEA Degrees → Decimal");

    TEST_CASE("lat 3912.3456 → 39.205760");
    double d = gps_nmea_to_degrees(3912.3456);
    TEST_ASSERT(fabs(d - 39.205760) < 0.0001, "lat conversion wrong");
    TEST_OK();

    TEST_CASE("lon 11623.4567 → 116.390945");
    d = gps_nmea_to_degrees(11623.4567);
    TEST_ASSERT(fabs(d - 116.390945) < 0.0001, "lon conversion wrong");
    TEST_OK();

    TEST_CASE("equator 0.0 → 0.0");
    d = gps_nmea_to_degrees(0.0);
    TEST_ASSERT(fabs(d) < 0.0001, "zero should stay zero");
    TEST_OK();
}

/* ---- 测试 2: Haversine ---- */
static void test_haversine(void)
{
    TEST_START("Haversine Distance");

    /* 北京天安门 → 北京故宫 (约 1.5km) */
    TEST_CASE("Tiananmen → Forbidden City (~1.5km)");
    double d = gps_haversine_distance(39.9042, 116.3974,   /* 天安门 */
                                       39.9163, 116.3972);  /* 故宫 */
    printf("(%.0fm) ", d);
    TEST_ASSERT(d > 1000 && d < 2000, "distance should be ~1.3km");
    TEST_OK();

    /* 同一点 → 距离为 0 */
    TEST_CASE("same point → 0m");
    d = gps_haversine_distance(39.9, 116.4, 39.9, 116.4);
    TEST_ASSERT(d < 0.1, "same point distance should be ~0");
    TEST_OK();
}

/* ---- 测试 3: GGA 解析 ---- */
static void test_gga_parsing(void)
{
    TEST_START("NMEA GGA Parsing");

    /* 标准 GGA 语句 (定位有效) */
    const char *gga = "$GPGGA,123519.00,3912.3456,N,11623.4567,E,"
                      "1,08,0.9,50.5,M,0.0,M,,*4A\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("GGA full parse → 1 complete sentence");
    int ret = feed_sentence(&parser, gga);
    TEST_ASSERT(ret == 1, "should complete 1 sentence");
    TEST_OK();

    TEST_CASE("GGA type identification");
    TEST_ASSERT(parser.result.type == NMEA_GPGGA, "should be GGA");
    TEST_OK();

    TEST_CASE("GGA checksum OK");
    TEST_ASSERT(parser.result.checksum_ok == 1, "checksum should pass");
    TEST_OK();

    TEST_CASE("GGA data extraction: lat/lon/quality/sats");
    nmea_extract_gga(&parser.result, &gps);
    TEST_ASSERT(gps.hour == 12 && gps.min == 35 && gps.sec == 19,
                "time wrong");
    TEST_ASSERT(fabs(gps.latitude - 39.205760) < 0.001, "lat wrong");
    TEST_ASSERT(fabs(gps.longitude - 116.390945) < 0.001, "lon wrong");
    TEST_ASSERT(gps.fix_quality == 1, "fix quality should be 1");
    TEST_ASSERT(gps.satellites == 8, "sats should be 8");
    TEST_ASSERT(fabs(gps.hdop - 0.9) < 0.01, "HDOP wrong");
    TEST_ASSERT(fabs(gps.altitude - 50.5) < 0.01, "altitude wrong");
    printf("(T=%02d:%02d:%02d Lat=%.4f Lon=%.4f Fix=%d Sats=%d) ",
           gps.hour, gps.min, gps.sec,
           gps.latitude, gps.longitude,
           gps.fix_quality, gps.satellites);
    TEST_OK();
}

/* ---- 测试 4: RMC 解析 ---- */
static void test_rmc_parsing(void)
{
    TEST_START("NMEA RMC Parsing");

    const char *rmc = "$GPRMC,123519.00,A,3912.3456,N,11623.4567,E,"
                      "15.5,180.0,010826,,,A*6E\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("RMC full parse");
    int ret = feed_sentence(&parser, rmc);
    TEST_ASSERT(ret == 1, "should complete");
    TEST_ASSERT(parser.result.type == NMEA_GPRMC, "should be RMC");
    TEST_OK();

    TEST_CASE("RMC speed 15.5 knots → 28.7 km/h");
    nmea_extract_rmc(&parser.result, &gps);
    TEST_ASSERT(fabs(gps.speed - 28.706) < 0.1, "speed conversion wrong");
    printf("(%.1fkm/h) ", gps.speed);
    TEST_OK();

    TEST_CASE("RMC date 010826 → 2026-08-01");
    TEST_ASSERT(gps.year == 2026, "year wrong");
    TEST_ASSERT(gps.month == 8, "month wrong");
    TEST_ASSERT(gps.day == 1, "day wrong");
    printf("(%04d-%02d-%02d) ", gps.year, gps.month, gps.day);
    TEST_OK();
}

/* ---- 测试 5: 校验和错误检测 ---- */
static void test_checksum_error(void)
{
    TEST_START("NMEA Checksum Error Detection");

    /* 篡改的 GGA (校验和不匹配) */
    const char *bad = "$GPGGA,123519.00,3912.3456,N,11623.4567,E,"
                      "1,08,0.9,50.5,M,0.0,M,,*FF\r\n";  /* FF 是错误校验和 */

    nmea_parser_t parser;
    nmea_parser_init(&parser);

    TEST_CASE("corrupted checksum → rejected");
    int ret = feed_sentence(&parser, bad);
    TEST_ASSERT(ret == -1, "should return -1 (checksum error)");
    TEST_ASSERT(parser.result.checksum_ok == 0, "checksum should fail");
    TEST_ASSERT(parser.checksum_errors == 1, "error counter should increment");
    TEST_OK();
}

/* ---- 测试 6: 空字段容错 ---- */
static void test_empty_fields(void)
{
    TEST_START("Empty Field Tolerance");

    /* GPS 刚启动时某些字段为空 */
    const char *partial = "$GPGGA,,,,,,0,00,,,,,,,*66\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("empty fields → no crash");
    int ret = feed_sentence(&parser, partial);
    TEST_ASSERT(ret == 1, "should still complete");
    nmea_extract_gga(&parser.result, &gps);
    /* 所有字段应为 0 (默认值), 不应该崩溃 */
    TEST_ASSERT(gps.fix_quality == 0, "quality should be 0");
    TEST_ASSERT(gps.satellites == 0, "sats should be 0");
    TEST_OK();
}

/* ---- 测试 7: GSV 卫星信息 ---- */
static void test_gsv_parsing(void)
{
    TEST_START("NMEA GSV Parsing");

    const char *gsv = "$GPGSV,3,1,12,01,45,180,42,02,30,090,38,"
                      "03,60,270,45,04,15,360,30*7B\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("GSV: 12 satellites in view");
    int ret = feed_sentence(&parser, gsv);
    TEST_ASSERT(ret == 1, "should complete");
    TEST_ASSERT(parser.result.type == NMEA_GPGSV, "should be GSV");
    nmea_extract_gsv(&parser.result, &gps);
    TEST_ASSERT(gps.satellites == 12, "total sats should be 12");
    TEST_OK();
}

/* ---- 测试 8: GLL 地理定位信息 ---- */
static void test_gll_parsing(void)
{
    TEST_START("NMEA GLL Parsing");

    /* $GPGLL,纬度,N,经度,E,时间,状态,模式*校验和 */
    const char *gll = "$GPGLL,4005.22599,N,11632.58234,E,082559.00,A,A*7A\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("GLL full parse → type identified");
    int ret = feed_sentence(&parser, gll);
    TEST_ASSERT(ret == 1, "should complete");
    TEST_ASSERT(parser.result.type == NMEA_GPGLL, "should be GLL");
    TEST_OK();

    TEST_CASE("GLL data extraction: lat/lon/time");
    nmea_extract_gll(&parser.result, &gps);
    TEST_ASSERT(gps.hour == 8 && gps.min == 25 && gps.sec == 59, "time wrong");
    TEST_ASSERT(fabs(gps.latitude - 40.087100) < 0.0001, "lat wrong");
    TEST_ASSERT(fabs(gps.longitude - 116.543039) < 0.0001, "lon wrong");
    printf("(T=%02d:%02d:%02d Lat=%.6f Lon=%.6f) ",
           gps.hour, gps.min, gps.sec, gps.latitude, gps.longitude);
    TEST_OK();

    /* 无效 GLL (状态=V) */
    TEST_CASE("GLL with status=V → fix_quality=0");
    const char *bad_gll = "$GPGLL,4005.22599,N,11632.58234,E,082559.00,V,A*7B\r\n";
    nmea_parser_t parser2;
    nmea_parser_init(&parser2);
    gps_data_t gps2;
    memset(&gps2, 0, sizeof(gps2));
    feed_sentence(&parser2, bad_gll);
    nmea_extract_gll(&parser2.result, &gps2);
    TEST_ASSERT(gps2.fix_quality == 0, "invalid GLL should set quality=0");
    TEST_OK();
}

/* ---- 测试 9: VTG 地面速度信息 ---- */
static void test_vtg_parsing(void)
{
    TEST_START("NMEA VTG Parsing");

    /* $GPVTG,航向(真),T,航向(磁),M,速度(节),N,速度(km/h),K,模式*校验和 */
    const char *vtg = "$GPVTG,180.0,T,185.0,M,15.5,N,28.7,K,A*3F\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("VTG full parse → speed 28.7 km/h (direct)");
    int ret = feed_sentence(&parser, vtg);
    TEST_ASSERT(ret == 1, "should complete");
    TEST_ASSERT(parser.result.type == NMEA_GPVTG, "should be VTG");
    nmea_extract_vtg(&parser.result, &gps);
    TEST_ASSERT(fabs(gps.speed - 28.7) < 0.01, "speed (km/h direct) wrong");
    printf("(%.1f km/h) ", gps.speed);
    TEST_OK();

    /* VTG 只有节, 没有km/h → 应回退到节转换 */
    TEST_CASE("VTG knots-only → fallback conversion");
    const char *vtg_knots = "$GPVTG,90.0,T,95.0,M,10.0,N,,K,A*28\r\n";
    nmea_parser_t parser2;
    nmea_parser_init(&parser2);
    gps_data_t gps2;
    memset(&gps2, 0, sizeof(gps2));
    feed_sentence(&parser2, vtg_knots);
    nmea_extract_vtg(&parser2.result, &gps2);
    TEST_ASSERT(fabs(gps2.speed - 18.52) < 0.1, "knot fallback conversion wrong");
    printf("(%.1f km/h from knots) ", gps2.speed);
    TEST_OK();
}

/* ---- 测试 10: GSA 精度因子 ---- */
static void test_gsa_parsing(void)
{
    TEST_START("NMEA GSA Parsing");

    /* $GPGSA,模式,定位类型,PRN1..12,PDOP,HDOP,VDOP*校验和 */
    const char *gsa = "$GPGSA,A,3,01,02,03,04,05,06,,,,,,,1.5,0.9,1.2*3E\r\n";

    nmea_parser_t parser;
    nmea_parser_init(&parser);
    gps_data_t gps;
    memset(&gps, 0, sizeof(gps));

    TEST_CASE("GSA full parse → fix_type=3D, HDOP=0.9");
    int ret = feed_sentence(&parser, gsa);
    TEST_ASSERT(ret == 1, "should complete");
    TEST_ASSERT(parser.result.type == NMEA_GPGSA, "should be GSA");
    nmea_extract_gsa(&parser.result, &gps);
    TEST_ASSERT(gps.fix_quality == 3, "fix type should be 3 (3D)");
    TEST_ASSERT(fabs(gps.hdop - 0.9) < 0.01, "HDOP wrong");
    printf("(Fix=%d HDOP=%.1f) ", gps.fix_quality, gps.hdop);
    TEST_OK();

    /* GSA 无定位 (fix_type=1) */
    TEST_CASE("GSA fix_type=1 → no fix");
    const char *gsa_nofix = "$GPGSA,A,1,,,,,,,,,,,,,99.9,99.9,99.9*3E\r\n";
    nmea_parser_t parser2;
    nmea_parser_init(&parser2);
    gps_data_t gps2;
    memset(&gps2, 0, sizeof(gps2));
    feed_sentence(&parser2, gsa_nofix);
    nmea_extract_gsa(&parser2.result, &gps2);
    TEST_ASSERT(gps2.fix_quality == 1, "should be no fix (1)");
    TEST_OK();
}

/* ---- main ---- */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  gps_daemon — Module Unit Tests              ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    log_init(NULL, LOG_DEBUG, 0, 0);

    test_degrees_conversion();
    test_haversine();
    test_gga_parsing();
    test_rmc_parsing();
    test_checksum_error();
    test_empty_fields();
    test_gsv_parsing();
    test_gll_parsing();
    test_vtg_parsing();
    test_gsa_parsing();

    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Results: total=%d passed=%d failed=%d        ║\n",
           g_total, g_passed, g_failed);
    printf("╚══════════════════════════════════════════════╝\n");

    log_close();
    return g_failed > 0 ? 1 : 0;
}
