#ifndef     _KP_FONT_LIB_H_
#define     _KP_FONT_LIB_H_

//for base font lib

#define FLASH_FONT_HEAD_ADDR				0x100000 //字库配置 ，第1个字节为0x5A 表示字库存在

#define FLASH_FONT_U2G_TABLE_START_ADDR		0x100100 //UNICODE 转 GBK 表 起始地址
#define FLASH_FONT_U2G_TABLE_LEN			0x15480  //UNICODE 转 GBK 表 长度
#define FLASH_FONT_G2U_TABLE_START_ADDR		0x115580 //GBK 转 UNICODE 表 起始地址
#define FLASH_FONT_G2U_TABLE_LEN			0x15480  //GBK 转 UNICODE 表 长度
													 //0x22AA00 ~ 0x22FFFF  共5600个字节暂未使用
#define FLASH_FONT_GBK12_START_ADDR			0x130000 //GBK 12*12 点阵字库起始地址

#define FLASH_FONT_START_ADDR				0x100000 //字库功能 起始地址，1MB 空间循环刷新，字符串存储
#define	FLASH_FONT_END_ADDR					0x1FFFFF //字库功能 末尾地址





//-------------------------------------------------------------------------------------------------------
//for kp font lib

#define FLASH_FONTS_ADDR 0                              //字库文件在Flash中绝对起始地址


#ifndef FONT_CHT_TW
    #define FONTS_CHS_12X12_ADDR (FLASH_FONTS_ADDR + 0x42F00)     //中文12号字体，在Flash中绝对起始地址
    #define FONTS_ASCII_6X12_ADDR (FONTS_CHS_12X12_ADDR + 0x08D0) // ASCII，12号字体，在Flash中绝对起始地址  大小0x474
    #define FONTS_CHS_16X16_ADDR (FLASH_FONTS_ADDR + 0x3000)      //中文16号字体，在Flash中绝对起始地址
    #define FONTS_ASCII_8X16_ADDR (FONTS_CHS_16X16_ADDR + 0x0BC0) // ASCII，16号字体，在Flash中绝对起始地址  大小0x5f0
    #define USER_DEFINED_ADDR 0xF6000u
    #define USER_FONTS_CUSTOM_ADDR  0x8D000u   //使用该区域做内置字库下载存储区  0XC8000 0XC9000 0xCA000 三个区为空
    #define USER_FONTS_CUSTOM1_ADDR  0XC9000u   //使用该区域做内置字库下载存储区
    #define USER_FONTS_CUSTOM_CHS16X16_ADDR  0XC8000u
    
#else
    #define FONTS_CHT_12X12_ADDR  FLASH_FONTS_ADDR     //繁体中文12号字体，在Flash中绝对起始地址 结束位置：0x70200    big5编码范围 A440- EF00 
    #define FONTS_ASCII_6X12_ADDR (FLASH_FONTS_ADDR + 0x60A00u) // ASCII，12号字体，在Flash中绝对起始地址  大小0x474
    #define FONTS_ASCII_8X16_ADDR (FLASH_FONTS_ADDR + 0x61200u) // ASCII，16号字体，在Flash中绝对起始地址  大小0x5f0
    #define USER_DEFINED_ADDR 0xF6000u
    #define USER_FONTS_CUSTOM_ADDR  0xF5000u   //使用该区域做内置字库下载存储区
    #define FONTS_CHS_12X12_ADDR 0
    #define FONTS_CHS_16X16_ADDR 0
#endif





#endif 
