// The keymap the compositor gives every client: US, complete (no includes),
// so the program needs no XKB data on the machine it runs on. Generated with
// `XKB_CONFIG_ROOT=<xkeyboard-config> xdesk --keymap us` (xkeyboard-config 2.41).
module xdesk.compositor;

import std;

namespace xdesk::compositor {

std::string_view builtin_keymap() {
    static constexpr std::string_view text = R"xkb(
xkb_keymap {
xkb_keycodes "evdev_aliases(qwerty)" {
	minimum = 8;
	maximum = 708;
	<ESC> = 9;
	<AE01> = 10;
	<AE02> = 11;
	<AE03> = 12;
	<AE04> = 13;
	<AE05> = 14;
	<AE06> = 15;
	<AE07> = 16;
	<AE08> = 17;
	<AE09> = 18;
	<AE10> = 19;
	<AE11> = 20;
	<AE12> = 21;
	<BKSP> = 22;
	<TAB> = 23;
	<AD01> = 24;
	<AD02> = 25;
	<AD03> = 26;
	<AD04> = 27;
	<AD05> = 28;
	<AD06> = 29;
	<AD07> = 30;
	<AD08> = 31;
	<AD09> = 32;
	<AD10> = 33;
	<AD11> = 34;
	<AD12> = 35;
	<RTRN> = 36;
	<LCTL> = 37;
	<AC01> = 38;
	<AC02> = 39;
	<AC03> = 40;
	<AC04> = 41;
	<AC05> = 42;
	<AC06> = 43;
	<AC07> = 44;
	<AC08> = 45;
	<AC09> = 46;
	<AC10> = 47;
	<AC11> = 48;
	<TLDE> = 49;
	<LFSH> = 50;
	<BKSL> = 51;
	<AB01> = 52;
	<AB02> = 53;
	<AB03> = 54;
	<AB04> = 55;
	<AB05> = 56;
	<AB06> = 57;
	<AB07> = 58;
	<AB08> = 59;
	<AB09> = 60;
	<AB10> = 61;
	<RTSH> = 62;
	<KPMU> = 63;
	<LALT> = 64;
	<SPCE> = 65;
	<CAPS> = 66;
	<FK01> = 67;
	<FK02> = 68;
	<FK03> = 69;
	<FK04> = 70;
	<FK05> = 71;
	<FK06> = 72;
	<FK07> = 73;
	<FK08> = 74;
	<FK09> = 75;
	<FK10> = 76;
	<NMLK> = 77;
	<SCLK> = 78;
	<KP7> = 79;
	<KP8> = 80;
	<KP9> = 81;
	<KPSU> = 82;
	<KP4> = 83;
	<KP5> = 84;
	<KP6> = 85;
	<KPAD> = 86;
	<KP1> = 87;
	<KP2> = 88;
	<KP3> = 89;
	<KP0> = 90;
	<KPDL> = 91;
	<LVL3> = 92;
	<LSGT> = 94;
	<FK11> = 95;
	<FK12> = 96;
	<AB11> = 97;
	<KATA> = 98;
	<HIRA> = 99;
	<HENK> = 100;
	<HKTG> = 101;
	<MUHE> = 102;
	<JPCM> = 103;
	<KPEN> = 104;
	<RCTL> = 105;
	<KPDV> = 106;
	<PRSC> = 107;
	<RALT> = 108;
	<LNFD> = 109;
	<HOME> = 110;
	<UP> = 111;
	<PGUP> = 112;
	<LEFT> = 113;
	<RGHT> = 114;
	<END> = 115;
	<DOWN> = 116;
	<PGDN> = 117;
	<INS> = 118;
	<DELE> = 119;
	<I120> = 120;
	<MUTE> = 121;
	<VOL-> = 122;
	<VOL+> = 123;
	<POWR> = 124;
	<KPEQ> = 125;
	<I126> = 126;
	<PAUS> = 127;
	<I128> = 128;
	<I129> = 129;
	<HNGL> = 130;
	<HJCV> = 131;
	<AE13> = 132;
	<LWIN> = 133;
	<RWIN> = 134;
	<COMP> = 135;
	<STOP> = 136;
	<AGAI> = 137;
	<PROP> = 138;
	<UNDO> = 139;
	<FRNT> = 140;
	<COPY> = 141;
	<OPEN> = 142;
	<PAST> = 143;
	<FIND> = 144;
	<CUT> = 145;
	<HELP> = 146;
	<I147> = 147;
	<I148> = 148;
	<I149> = 149;
	<I150> = 150;
	<I151> = 151;
	<I152> = 152;
	<I153> = 153;
	<I154> = 154;
	<I155> = 155;
	<I156> = 156;
	<I157> = 157;
	<I158> = 158;
	<I159> = 159;
	<I160> = 160;
	<I161> = 161;
	<I162> = 162;
	<I163> = 163;
	<I164> = 164;
	<I165> = 165;
	<I166> = 166;
	<I167> = 167;
	<I168> = 168;
	<I169> = 169;
	<I170> = 170;
	<I171> = 171;
	<I172> = 172;
	<I173> = 173;
	<I174> = 174;
	<I175> = 175;
	<I176> = 176;
	<I177> = 177;
	<I178> = 178;
	<I179> = 179;
	<I180> = 180;
	<I181> = 181;
	<I182> = 182;
	<I183> = 183;
	<I184> = 184;
	<I185> = 185;
	<I186> = 186;
	<I187> = 187;
	<I188> = 188;
	<I189> = 189;
	<I190> = 190;
	<FK13> = 191;
	<FK14> = 192;
	<FK15> = 193;
	<FK16> = 194;
	<FK17> = 195;
	<FK18> = 196;
	<FK19> = 197;
	<FK20> = 198;
	<FK21> = 199;
	<FK22> = 200;
	<FK23> = 201;
	<FK24> = 202;
	<LVL5> = 203;
	<ALT> = 204;
	<META> = 205;
	<SUPR> = 206;
	<HYPR> = 207;
	<I208> = 208;
	<I209> = 209;
	<I210> = 210;
	<I211> = 211;
	<I212> = 212;
	<I213> = 213;
	<I214> = 214;
	<I215> = 215;
	<I216> = 216;
	<I217> = 217;
	<I218> = 218;
	<I219> = 219;
	<I220> = 220;
	<I221> = 221;
	<I222> = 222;
	<I223> = 223;
	<I224> = 224;
	<I225> = 225;
	<I226> = 226;
	<I227> = 227;
	<I228> = 228;
	<I229> = 229;
	<I230> = 230;
	<I231> = 231;
	<I232> = 232;
	<I233> = 233;
	<I234> = 234;
	<I235> = 235;
	<I236> = 236;
	<I237> = 237;
	<I238> = 238;
	<I239> = 239;
	<I240> = 240;
	<I241> = 241;
	<I242> = 242;
	<I243> = 243;
	<I244> = 244;
	<I245> = 245;
	<I246> = 246;
	<I247> = 247;
	<I248> = 248;
	<I249> = 249;
	<I250> = 250;
	<I251> = 251;
	<I252> = 252;
	<I253> = 253;
	<I254> = 254;
	<I255> = 255;
	<I256> = 256;
	<I360> = 360;
	<I361> = 361;
	<I362> = 362;
	<I363> = 363;
	<I364> = 364;
	<I365> = 365;
	<I366> = 366;
	<I367> = 367;
	<I368> = 368;
	<I369> = 369;
	<I370> = 370;
	<I371> = 371;
	<I372> = 372;
	<I373> = 373;
	<I374> = 374;
	<I375> = 375;
	<I376> = 376;
	<I377> = 377;
	<I378> = 378;
	<I379> = 379;
	<I380> = 380;
	<I381> = 381;
	<I382> = 382;
	<I383> = 383;
	<I384> = 384;
	<I385> = 385;
	<I386> = 386;
	<I387> = 387;
	<I388> = 388;
	<I389> = 389;
	<I390> = 390;
	<I391> = 391;
	<I392> = 392;
	<I393> = 393;
	<I394> = 394;
	<I395> = 395;
	<I396> = 396;
	<I397> = 397;
	<I398> = 398;
	<I399> = 399;
	<I400> = 400;
	<I401> = 401;
	<I402> = 402;
	<I403> = 403;
	<I404> = 404;
	<I405> = 405;
	<I406> = 406;
	<I407> = 407;
	<I408> = 408;
	<I409> = 409;
	<I410> = 410;
	<I411> = 411;
	<I412> = 412;
	<I413> = 413;
	<I414> = 414;
	<I415> = 415;
	<I416> = 416;
	<I417> = 417;
	<I418> = 418;
	<I419> = 419;
	<I420> = 420;
	<I421> = 421;
	<I422> = 422;
	<I423> = 423;
	<I424> = 424;
	<I425> = 425;
	<I426> = 426;
	<I427> = 427;
	<I428> = 428;
	<I429> = 429;
	<I430> = 430;
	<I431> = 431;
	<I432> = 432;
	<I433> = 433;
	<I434> = 434;
	<I435> = 435;
	<I436> = 436;
	<I437> = 437;
	<I438> = 438;
	<I439> = 439;
	<I440> = 440;
	<I441> = 441;
	<I442> = 442;
	<I443> = 443;
	<I444> = 444;
	<I445> = 445;
	<I446> = 446;
	<I447> = 447;
	<I448> = 448;
	<I449> = 449;
	<I450> = 450;
	<I452> = 452;
	<I453> = 453;
	<I454> = 454;
	<I456> = 456;
	<I457> = 457;
	<I458> = 458;
	<I459> = 459;
	<I472> = 472;
	<I473> = 473;
	<I474> = 474;
	<I475> = 475;
	<I476> = 476;
	<I477> = 477;
	<I478> = 478;
	<I479> = 479;
	<I480> = 480;
	<I481> = 481;
	<I482> = 482;
	<I483> = 483;
	<I484> = 484;
	<I485> = 485;
	<I486> = 486;
	<I487> = 487;
	<I488> = 488;
	<I489> = 489;
	<I490> = 490;
	<I491> = 491;
	<I492> = 492;
	<I493> = 493;
	<I505> = 505;
	<I506> = 506;
	<I507> = 507;
	<I508> = 508;
	<I509> = 509;
	<I510> = 510;
	<I511> = 511;
	<I512> = 512;
	<I513> = 513;
	<I514> = 514;
	<I520> = 520;
	<I521> = 521;
	<I522> = 522;
	<I523> = 523;
	<I524> = 524;
	<I525> = 525;
	<I526> = 526;
	<I527> = 527;
	<I528> = 528;
	<I529> = 529;
	<I530> = 530;
	<I531> = 531;
	<I532> = 532;
	<I533> = 533;
	<I534> = 534;
	<I535> = 535;
	<I536> = 536;
	<I537> = 537;
	<I538> = 538;
	<I539> = 539;
	<I540> = 540;
	<I541> = 541;
	<I542> = 542;
	<I543> = 543;
	<I544> = 544;
	<I545> = 545;
	<I546> = 546;
	<I547> = 547;
	<I548> = 548;
	<I549> = 549;
	<I550> = 550;
	<I568> = 568;
	<I569> = 569;
	<I584> = 584;
	<I585> = 585;
	<I586> = 586;
	<I587> = 587;
	<I588> = 588;
	<I589> = 589;
	<I590> = 590;
	<I591> = 591;
	<I592> = 592;
	<I593> = 593;
	<I594> = 594;
	<I595> = 595;
	<I596> = 596;
	<I597> = 597;
	<I600> = 600;
	<I601> = 601;
	<I616> = 616;
	<I617> = 617;
	<I618> = 618;
	<I619> = 619;
	<I620> = 620;
	<I621> = 621;
	<I622> = 622;
	<I623> = 623;
	<I624> = 624;
	<I625> = 625;
	<I626> = 626;
	<I627> = 627;
	<I628> = 628;
	<I629> = 629;
	<I630> = 630;
	<I631> = 631;
	<I632> = 632;
	<I633> = 633;
	<I634> = 634;
	<I635> = 635;
	<I636> = 636;
	<I637> = 637;
	<I638> = 638;
	<I639> = 639;
	<I640> = 640;
	<I641> = 641;
	<I642> = 642;
	<I643> = 643;
	<I644> = 644;
	<I645> = 645;
	<I646> = 646;
	<I647> = 647;
	<I648> = 648;
	<I649> = 649;
	<I650> = 650;
	<I651> = 651;
	<I652> = 652;
	<I653> = 653;
	<I654> = 654;
	<I655> = 655;
	<I656> = 656;
	<I657> = 657;
	<I664> = 664;
	<I665> = 665;
	<I666> = 666;
	<I667> = 667;
	<I668> = 668;
	<I669> = 669;
	<I670> = 670;
	<I671> = 671;
	<I672> = 672;
	<I673> = 673;
	<I674> = 674;
	<I675> = 675;
	<I676> = 676;
	<I677> = 677;
	<I678> = 678;
	<I679> = 679;
	<I680> = 680;
	<I681> = 681;
	<I682> = 682;
	<I683> = 683;
	<I684> = 684;
	<I685> = 685;
	<I686> = 686;
	<I687> = 687;
	<I688> = 688;
	<I689> = 689;
	<I690> = 690;
	<I691> = 691;
	<I692> = 692;
	<I693> = 693;
	<I696> = 696;
	<I697> = 697;
	<I698> = 698;
	<I699> = 699;
	<I700> = 700;
	<I701> = 701;
	<I704> = 704;
	<I705> = 705;
	<I706> = 706;
	<I707> = 707;
	<I708> = 708;
	indicator 1 = "Caps Lock";
	indicator 2 = "Num Lock";
	indicator 3 = "Scroll Lock";
	indicator 4 = "Compose";
	indicator 5 = "Kana";
	indicator 6 = "Sleep";
	indicator 7 = "Suspend";
	indicator 8 = "Mute";
	indicator 9 = "Misc";
	indicator 10 = "Mail";
	indicator 11 = "Charging";
	indicator 12 = "Shift Lock";
	indicator 13 = "Group 2";
	indicator 14 = "Mouse Keys";
	alias <AC12> = <BKSL>;
	alias <ALGR> = <RALT>;
	alias <MENU> = <COMP>;
	alias <HZTG> = <TLDE>;
	alias <LMTA> = <LWIN>;
	alias <RMTA> = <RWIN>;
	alias <OUTP> = <I235>;
	alias <KITG> = <I236>;
	alias <KIDN> = <I237>;
	alias <KIUP> = <I238>;
	alias <I121> = <MUTE>;
	alias <I122> = <VOL->;
	alias <I123> = <VOL+>;
	alias <I124> = <POWR>;
	alias <I125> = <KPEQ>;
	alias <I127> = <PAUS>;
	alias <I130> = <HNGL>;
	alias <I131> = <HJCV>;
	alias <I132> = <AE13>;
	alias <I133> = <LWIN>;
	alias <I134> = <RWIN>;
	alias <I135> = <COMP>;
	alias <I136> = <STOP>;
	alias <I137> = <AGAI>;
	alias <I138> = <PROP>;
	alias <I139> = <UNDO>;
	alias <I140> = <FRNT>;
	alias <I141> = <COPY>;
	alias <I142> = <OPEN>;
	alias <I143> = <PAST>;
	alias <I144> = <FIND>;
	alias <I145> = <CUT>;
	alias <I146> = <HELP>;
	alias <I191> = <FK13>;
	alias <I192> = <FK14>;
	alias <I193> = <FK15>;
	alias <I194> = <FK16>;
	alias <I195> = <FK17>;
	alias <I196> = <FK18>;
	alias <I197> = <FK19>;
	alias <I198> = <FK20>;
	alias <I199> = <FK21>;
	alias <I200> = <FK22>;
	alias <I201> = <FK23>;
	alias <I202> = <FK24>;
	alias <MDSW> = <LVL5>;
	alias <KPPT> = <I129>;
	alias <LatQ> = <AD01>;
	alias <LatW> = <AD02>;
	alias <LatE> = <AD03>;
	alias <LatR> = <AD04>;
	alias <LatT> = <AD05>;
	alias <LatY> = <AD06>;
	alias <LatU> = <AD07>;
	alias <LatI> = <AD08>;
	alias <LatO> = <AD09>;
	alias <LatP> = <AD10>;
	alias <LatA> = <AC01>;
	alias <LatS> = <AC02>;
	alias <LatD> = <AC03>;
	alias <LatF> = <AC04>;
	alias <LatG> = <AC05>;
	alias <LatH> = <AC06>;
	alias <LatJ> = <AC07>;
	alias <LatK> = <AC08>;
	alias <LatL> = <AC09>;
	alias <LatZ> = <AB01>;
	alias <LatX> = <AB02>;
	alias <LatC> = <AB03>;
	alias <LatV> = <AB04>;
	alias <LatB> = <AB05>;
	alias <LatN> = <AB06>;
	alias <LatM> = <AB07>;
};

xkb_types "complete" {
	virtual_modifiers NumLock,Alt,LevelThree,LevelFive,Meta,Super,Hyper,ScrollLock=0x8000;

	type "ONE_LEVEL" {
		modifiers= none;
		level_name[1]= "Any";
	};
	type "TWO_LEVEL" {
		modifiers= Shift;
		map[Shift]= 2;
		level_name[1]= "Base";
		level_name[2]= "Shift";
	};
	type "ALPHABETIC" {
		modifiers= Shift+Lock;
		map[Shift]= 2;
		map[Lock]= 2;
		level_name[1]= "Base";
		level_name[2]= "Caps";
	};
	type "PC_ALT_LEVEL2" {
		modifiers= Alt;
		map[Alt]= 2;
		level_name[1]= "Base";
		level_name[2]= "Alt";
	};
	type "PC_CONTROL_LEVEL2" {
		modifiers= Control;
		map[Control]= 2;
		level_name[1]= "Base";
		level_name[2]= "Control";
	};
	type "CTRL+ALT" {
		modifiers= Shift+Control+Alt+LevelThree;
		map[Shift]= 2;
		preserve[Shift]= Shift;
		map[LevelThree]= 3;
		map[Shift+LevelThree]= 4;
		preserve[Shift+LevelThree]= Shift;
		map[Control+Alt]= 5;
		level_name[1]= "Base";
		level_name[2]= "Shift";
		level_name[3]= "AltGr";
		level_name[4]= "Shift AltGr";
		level_name[5]= "Ctrl+Alt";
	};
	type "FOUR_LEVEL" {
		modifiers= Shift+LevelThree;
		map[Shift]= 2;
		map[LevelThree]= 3;
		map[Shift+LevelThree]= 4;
		level_name[1]= "Base";
		level_name[2]= "Shift";
		level_name[3]= "AltGr";
		level_name[4]= "Shift AltGr";
	};
	type "KEYPAD" {
		modifiers= Shift+NumLock;
		map[NumLock]= 2;
		level_name[1]= "Base";
		level_name[2]= "Number";
	};
};

xkb_compatibility "complete" {
	virtual_modifiers NumLock,Alt,LevelThree,LevelFive,Meta,Super,Hyper,ScrollLock=0x8000;

	interpret.useModMapMods= AnyLevel;
	interpret.repeat= False;
	interpret 0xff7f+AnyOf(all) {
		virtualModifier= NumLock;
		action= LockMods(modifiers=NumLock);
	};
	interpret 0xfe03+AnyOf(all) {
		virtualModifier= LevelThree;
		useModMapMods=level1;
		action= SetMods(modifiers=LevelThree,clearLocks);
	};
	interpret 0xffe9+AnyOf(all) {
		virtualModifier= Alt;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffea+AnyOf(all) {
		virtualModifier= Alt;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffe7+AnyOf(all) {
		virtualModifier= Meta;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffe8+AnyOf(all) {
		virtualModifier= Meta;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffeb+AnyOf(all) {
		virtualModifier= Super;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffec+AnyOf(all) {
		virtualModifier= Super;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xffed+AnyOf(all) {
		virtualModifier= Hyper;
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	interpret 0xfe11+AnyOf(all) {
		virtualModifier= LevelFive;
		useModMapMods=level1;
		action= SetMods(modifiers=LevelFive,clearLocks);
	};
	interpret 0xfe08+AnyOfOrNone(all) {
		action= LockGroup(group=+1);
	};
	interpret 0xffb1+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=+1);
	};
	interpret 0xff9c+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=+1);
	};
	interpret 0xffb2+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+0,y=+1);
	};
	interpret 0xff99+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+0,y=+1);
	};
	interpret 0xffb3+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=+1);
	};
	interpret 0xff9b+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=+1);
	};
	interpret 0xffb4+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=+0);
	};
	interpret 0xff96+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=+0);
	};
	interpret 0xffb6+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=+0);
	};
	interpret 0xff98+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=+0);
	};
	interpret 0xffb7+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=-1);
	};
	interpret 0xff95+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=-1,y=-1);
	};
	interpret 0xffb8+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+0,y=-1);
	};
	interpret 0xff97+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+0,y=-1);
	};
	interpret 0xffb9+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=-1);
	};
	interpret 0xff9a+AnyOfOrNone(all) {
		repeat= True;
		action= MovePtr(x=+1,y=-1);
	};
	interpret 0xffb5+AnyOfOrNone(all) {
		repeat= True;
		action= PtrBtn(button=default);
	};
	interpret 0xff9d+AnyOfOrNone(all) {
		repeat= True;
		action= PtrBtn(button=default);
	};
	interpret 0xffaf+AnyOfOrNone(all) {
		repeat= True;
		action= SetPtrDflt(affect=button,button=1);
	};
	interpret 0xffaa+AnyOfOrNone(all) {
		repeat= True;
		action= SetPtrDflt(affect=button,button=2);
	};
	interpret 0xffad+AnyOfOrNone(all) {
		repeat= True;
		action= SetPtrDflt(affect=button,button=3);
	};
	interpret 0xffab+AnyOfOrNone(all) {
		repeat= True;
		action= PtrBtn(button=default,count=2);
	};
	interpret 0xffb0+AnyOfOrNone(all) {
		repeat= True;
		action= LockPtrBtn(button=default,affect=lock);
	};
	interpret 0xff9e+AnyOfOrNone(all) {
		repeat= True;
		action= LockPtrBtn(button=default,affect=lock);
	};
	interpret 0xffae+AnyOfOrNone(all) {
		repeat= True;
		action= LockPtrBtn(button=default,affect=unlock);
	};
	interpret 0xff9f+AnyOfOrNone(all) {
		repeat= True;
		action= LockPtrBtn(button=default,affect=unlock);
	};
	interpret 0xffe1+AnyOfOrNone(all) {
		action= SetMods(modifiers=Shift);
	};
	interpret 0xffe2+AnyOfOrNone(all) {
		action= SetMods(modifiers=Shift);
	};
	interpret 0x1008fe01+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=1,!same);
	};
	interpret 0x1008fe02+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=2,!same);
	};
	interpret 0x1008fe03+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=3,!same);
	};
	interpret 0x1008fe04+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=4,!same);
	};
	interpret 0x1008fe05+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=5,!same);
	};
	interpret 0x1008fe06+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=6,!same);
	};
	interpret 0x1008fe07+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=7,!same);
	};
	interpret 0x1008fe08+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=8,!same);
	};
	interpret 0x1008fe09+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=9,!same);
	};
	interpret 0x1008fe0a+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=10,!same);
	};
	interpret 0x1008fe0b+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=11,!same);
	};
	interpret 0x1008fe0c+AnyOfOrNone(all) {
		repeat= True;
		action= SwitchScreen(screen=12,!same);
	};
	interpret 0x1008fe22+AnyOfOrNone(all) {
		repeat= True;
		action= Private(type=0x86,data[0]=0x2b,data[1]=0x56,data[2]=0x4d,data[3]=0x6f,data[4]=0x64,data[5]=0x65,data[6]=0x00);
	};
	interpret 0x1008fe23+AnyOfOrNone(all) {
		repeat= True;
		action= Private(type=0x86,data[0]=0x2d,data[1]=0x56,data[2]=0x4d,data[3]=0x6f,data[4]=0x64,data[5]=0x65,data[6]=0x00);
	};
	interpret 0xffe5+AnyOfOrNone(all) {
		action= LockMods(modifiers=Lock);
	};
	interpret Any+AnyOf(all) {
		action= SetMods(modifiers=modMapMods,clearLocks);
	};
	indicator "Caps Lock" {
		whichModState= locked;
		modifiers= Lock;
	};
	indicator "Num Lock" {
		whichModState= locked;
		modifiers= NumLock;
	};
	indicator "Scroll Lock" {
		whichModState= locked;
		modifiers= ScrollLock;
	};
	indicator "Shift Lock" {
		whichModState= locked;
		modifiers= Shift;
	};
	indicator "Group 2" {
		groups= 0xfffffffe;
	};
	indicator "Mouse Keys" {
		controls= MouseKeys;
	};
};

xkb_symbols "pc_us_inet(evdev)" {
	name[1]="English (US)";

	key <ESC> {	[ 0xff1b ] };
	key <AE01> {	[ 0x31, 0x21 ] };
	key <AE02> {	[ 0x32, 0x40 ] };
	key <AE03> {	[ 0x33, 0x23 ] };
	key <AE04> {	[ 0x34, 0x24 ] };
	key <AE05> {	[ 0x35, 0x25 ] };
	key <AE06> {	[ 0x36, 0x5e ] };
	key <AE07> {	[ 0x37, 0x26 ] };
	key <AE08> {	[ 0x38, 0x2a ] };
	key <AE09> {	[ 0x39, 0x28 ] };
	key <AE10> {	[ 0x30, 0x29 ] };
	key <AE11> {	[ 0x2d, 0x5f ] };
	key <AE12> {	[ 0x3d, 0x2b ] };
	key <BKSP> {	[ 0xff08, 0xff08 ] };
	key <TAB> {	[ 0xff09, 0xfe20 ] };
	key <AD01> {	[ 0x71, 0x51 ] };
	key <AD02> {	[ 0x77, 0x57 ] };
	key <AD03> {	[ 0x65, 0x45 ] };
	key <AD04> {	[ 0x72, 0x52 ] };
	key <AD05> {	[ 0x74, 0x54 ] };
	key <AD06> {	[ 0x79, 0x59 ] };
	key <AD07> {	[ 0x75, 0x55 ] };
	key <AD08> {	[ 0x69, 0x49 ] };
	key <AD09> {	[ 0x6f, 0x4f ] };
	key <AD10> {	[ 0x70, 0x50 ] };
	key <AD11> {	[ 0x5b, 0x7b ] };
	key <AD12> {	[ 0x5d, 0x7d ] };
	key <RTRN> {	[ 0xff0d ] };
	key <LCTL> {	[ 0xffe3 ] };
	key <AC01> {	[ 0x61, 0x41 ] };
	key <AC02> {	[ 0x73, 0x53 ] };
	key <AC03> {	[ 0x64, 0x44 ] };
	key <AC04> {	[ 0x66, 0x46 ] };
	key <AC05> {	[ 0x67, 0x47 ] };
	key <AC06> {	[ 0x68, 0x48 ] };
	key <AC07> {	[ 0x6a, 0x4a ] };
	key <AC08> {	[ 0x6b, 0x4b ] };
	key <AC09> {	[ 0x6c, 0x4c ] };
	key <AC10> {	[ 0x3b, 0x3a ] };
	key <AC11> {	[ 0x27, 0x22 ] };
	key <TLDE> {	[ 0x60, 0x7e ] };
	key <LFSH> {	[ 0xffe1 ] };
	key <BKSL> {	[ 0x5c, 0x7c ] };
	key <AB01> {	[ 0x7a, 0x5a ] };
	key <AB02> {	[ 0x78, 0x58 ] };
	key <AB03> {	[ 0x63, 0x43 ] };
	key <AB04> {	[ 0x76, 0x56 ] };
	key <AB05> {	[ 0x62, 0x42 ] };
	key <AB06> {	[ 0x6e, 0x4e ] };
	key <AB07> {	[ 0x6d, 0x4d ] };
	key <AB08> {	[ 0x2c, 0x3c ] };
	key <AB09> {	[ 0x2e, 0x3e ] };
	key <AB10> {	[ 0x2f, 0x3f ] };
	key <RTSH> {	[ 0xffe2 ] };
	key <KPMU> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffaa, 0xffaa, 0xffaa, 0xffaa, 0x1008fe21 ]
	};
	key <LALT> {	[ 0xffe9, 0xffe7 ] };
	key <SPCE> {	[ 0x20 ] };
	key <CAPS> {	[ 0xffe5 ] };
	key <FK01> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffbe, 0xffbe, 0xffbe, 0xffbe, 0x1008fe01 ]
	};
	key <FK02> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffbf, 0xffbf, 0xffbf, 0xffbf, 0x1008fe02 ]
	};
	key <FK03> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc0, 0xffc0, 0xffc0, 0xffc0, 0x1008fe03 ]
	};
	key <FK04> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc1, 0xffc1, 0xffc1, 0xffc1, 0x1008fe04 ]
	};
	key <FK05> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc2, 0xffc2, 0xffc2, 0xffc2, 0x1008fe05 ]
	};
	key <FK06> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc3, 0xffc3, 0xffc3, 0xffc3, 0x1008fe06 ]
	};
	key <FK07> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc4, 0xffc4, 0xffc4, 0xffc4, 0x1008fe07 ]
	};
	key <FK08> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc5, 0xffc5, 0xffc5, 0xffc5, 0x1008fe08 ]
	};
	key <FK09> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc6, 0xffc6, 0xffc6, 0xffc6, 0x1008fe09 ]
	};
	key <FK10> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc7, 0xffc7, 0xffc7, 0xffc7, 0x1008fe0a ]
	};
	key <NMLK> {	[ 0xff7f ] };
	key <SCLK> {	[ 0xff14 ] };
	key <KP7> {	[ 0xff95, 0xffb7 ] };
	key <KP8> {	[ 0xff97, 0xffb8 ] };
	key <KP9> {	[ 0xff9a, 0xffb9 ] };
	key <KPSU> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffad, 0xffad, 0xffad, 0xffad, 0x1008fe23 ]
	};
	key <KP4> {	[ 0xff96, 0xffb4 ] };
	key <KP5> {	[ 0xff9d, 0xffb5 ] };
	key <KP6> {	[ 0xff98, 0xffb6 ] };
	key <KPAD> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffab, 0xffab, 0xffab, 0xffab, 0x1008fe22 ]
	};
	key <KP1> {	[ 0xff9c, 0xffb1 ] };
	key <KP2> {	[ 0xff99, 0xffb2 ] };
	key <KP3> {	[ 0xff9b, 0xffb3 ] };
	key <KP0> {	[ 0xff9e, 0xffb0 ] };
	key <KPDL> {	[ 0xff9f, 0xffae ] };
	key <LVL3> {	[ 0xfe03 ] };
	key <LSGT> {	[ 0x3c, 0x3e, 0x7c, 0xa6 ] };
	key <FK11> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc8, 0xffc8, 0xffc8, 0xffc8, 0x1008fe0b ]
	};
	key <FK12> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffc9, 0xffc9, 0xffc9, 0xffc9, 0x1008fe0c ]
	};
	key <KATA> {	[ 0xff26 ] };
	key <HIRA> {	[ 0xff25 ] };
	key <HENK> {	[ 0xff23 ] };
	key <HKTG> {	[ 0xff27 ] };
	key <MUHE> {	[ 0xff22 ] };
	key <KPEN> {	[ 0xff8d ] };
	key <RCTL> {	[ 0xffe4 ] };
	key <KPDV> {
		type= "CTRL+ALT",
		symbols[1]= [ 0xffaf, 0xffaf, 0xffaf, 0xffaf, 0x1008fe20 ]
	};
	key <PRSC> {
		type= "PC_ALT_LEVEL2",
		symbols[1]= [ 0xff61, 0xff15 ]
	};
	key <RALT> {	[ 0xffea, 0xffe8 ] };
	key <LNFD> {	[ 0xff0a ] };
	key <HOME> {	[ 0xff50 ] };
	key <UP> {	[ 0xff52 ] };
	key <PGUP> {	[ 0xff55 ] };
	key <LEFT> {	[ 0xff51 ] };
	key <RGHT> {	[ 0xff53 ] };
	key <END> {	[ 0xff57 ] };
	key <DOWN> {	[ 0xff54 ] };
	key <PGDN> {	[ 0xff56 ] };
	key <INS> {	[ 0xff63 ] };
	key <DELE> {	[ 0xffff ] };
	key <MUTE> {	[ 0x1008ff12 ] };
	key <VOL-> {	[ 0x1008ff11 ] };
	key <VOL+> {	[ 0x1008ff13 ] };
	key <POWR> {	[ 0x1008ff2a ] };
	key <KPEQ> {	[ 0xffbd ] };
	key <I126> {	[ 0xb1 ] };
	key <PAUS> {
		type= "PC_CONTROL_LEVEL2",
		symbols[1]= [ 0xff13, 0xff6b ]
	};
	key <I128> {	[ 0x1008ff4a ] };
	key <I129> {	[ 0xffae, 0xffae ] };
	key <HNGL> {	[ 0xff31 ] };
	key <HJCV> {	[ 0xff34 ] };
	key <LWIN> {	[ 0xffeb ] };
	key <RWIN> {	[ 0xffec ] };
	key <COMP> {	[ 0xff67 ] };
	key <STOP> {	[ 0xff69 ] };
	key <AGAI> {	[ 0xff66 ] };
	key <PROP> {	[ 0x1005ff70 ] };
	key <UNDO> {	[ 0xff65 ] };
	key <FRNT> {	[ 0x1005ff71 ] };
	key <COPY> {	[ 0x1008ff57 ] };
	key <OPEN> {	[ 0x1008ff6b ] };
	key <PAST> {	[ 0x1008ff6d ] };
	key <FIND> {	[ 0xff68 ] };
	key <CUT> {	[ 0x1008ff58 ] };
	key <HELP> {	[ 0xff6a ] };
	key <I147> {	[ 0x1008ff65 ] };
	key <I148> {	[ 0x1008ff1d ] };
	key <I150> {	[ 0x1008ff2f ] };
	key <I151> {	[ 0x1008ff2b ] };
	key <I152> {	[ 0x1008ff5d ] };
	key <I153> {	[ 0x1008ff7b ] };
	key <I155> {	[ 0x1008ff8a ] };
	key <I156> {	[ 0x1008ff41 ] };
	key <I157> {	[ 0x1008ff42 ] };
	key <I158> {	[ 0x1008ff2e ] };
	key <I159> {	[ 0x1008ff5a ] };
	key <I160> {	[ 0x1008ff2d ] };
	key <I161> {	[ 0x1008ff74 ] };
	key <I162> {	[ 0x1008ff7f ] };
	key <I163> {	[ 0x1008ff19 ] };
	key <I164> {	[ 0x1008ff30 ] };
	key <I165> {	[ 0x1008ff33 ] };
	key <I166> {	[ 0x1008ff26 ] };
	key <I167> {	[ 0x1008ff27 ] };
	key <I169> {	[ 0x1008ff2c ] };
	key <I170> {	[ 0x1008ff2c ] };
	key <I171> {	[ 0x1008ff17 ] };
	key <I172> {	[ 0x1008ff14, 0x1008ff31 ] };
	key <I173> {	[ 0x1008ff16 ] };
	key <I174> {	[ 0x1008ff15, 0x1008ff2c ] };
	key <I175> {	[ 0x1008ff1c ] };
	key <I176> {	[ 0x1008ff3e ] };
	key <I177> {	[ 0x1008ff6e ] };
	key <I179> {	[ 0x1008ff81 ] };
	key <I180> {	[ 0x1008ff18 ] };
	key <I181> {	[ 0x1008ff73 ] };
	key <I182> {	[ 0x1008ff56 ] };
	key <I185> {	[ 0x1008ff78 ] };
	key <I186> {	[ 0x1008ff79 ] };
	key <I187> {	[ 0x28 ] };
	key <I188> {	[ 0x29 ] };
	key <I189> {	[ 0x1008ff68 ] };
	key <I190> {	[ 0xff66 ] };
	key <FK13> {	[ 0x1008ff81 ] };
	key <FK14> {	[ 0x1008ff45 ] };
	key <FK15> {	[ 0x1008ff46 ] };
	key <FK16> {	[ 0x1008ff47 ] };
	key <FK17> {	[ 0x1008ff48 ] };
	key <FK18> {	[ 0x1008ff49 ] };
	key <FK20> {	[ 0x1008ffb2 ] };
	key <FK21> {	[ 0x1008ffa9 ] };
	key <FK22> {	[ 0x1008ffb0 ] };
	key <FK23> {	[ 0x1008ffb1 ] };
	key <LVL5> {	[ 0xfe11 ] };
	key <ALT> {	[ NoSymbol, 0xffe9 ] };
	key <META> {	[ NoSymbol, 0xffe7 ] };
	key <SUPR> {	[ NoSymbol, 0xffeb ] };
	key <HYPR> {	[ NoSymbol, 0xffed ] };
	key <I208> {	[ 0x1008ff14 ] };
	key <I209> {	[ 0x1008ff31 ] };
	key <I210> {	[ 0x1008ff43 ] };
	key <I211> {	[ 0x1008ff44 ] };
	key <I212> {	[ 0x1008ff4b ] };
	key <I213> {	[ 0x1008ffa7 ] };
	key <I214> {	[ 0x1008ff56 ] };
	key <I215> {	[ 0x1008ff14 ] };
	key <I216> {	[ 0x1008ff97 ] };
	key <I218> {	[ 0xff61 ] };
	key <I220> {	[ 0x1008ff8f ] };
	key <I221> {	[ 0x1008ffb6 ] };
	key <I223> {	[ 0x1008ff19 ] };
	key <I224> {	[ 0x1008ff8e ] };
	key <I225> {	[ 0x1008ff1b ] };
	key <I226> {	[ 0x1008ff5f ] };
	key <I227> {	[ 0x1008ff3c ] };
	key <I228> {	[ 0x1008ff5e ] };
	key <I229> {	[ 0x1008ff36 ] };
	key <I231> {	[ 0xff69 ] };
	key <I232> {	[ 0x1008ff03 ] };
	key <I233> {	[ 0x1008ff02 ] };
	key <I234> {	[ 0x1008ff32 ] };
	key <I235> {	[ 0x1008ff59 ] };
	key <I236> {	[ 0x1008ff04 ] };
	key <I237> {	[ 0x1008ff06 ] };
	key <I238> {	[ 0x1008ff05 ] };
	key <I239> {	[ 0x1008ff7b ] };
	key <I240> {	[ 0x1008ff72 ] };
	key <I241> {	[ 0x1008ff90 ] };
	key <I242> {	[ 0x1008ff77 ] };
	key <I243> {	[ 0x1008ff5b ] };
	key <I244> {	[ 0x1008ff93 ] };
	key <I245> {	[ 0x1008ff94 ] };
	key <I246> {	[ 0x1008ff95 ] };
	key <I247> {	[ 0x1008ff96 ] };
	key <I249> {	[ 0x1008fe22 ] };
	key <I250> {	[ 0x1008fe23 ] };
	key <I251> {	[ 0x1008ff07 ] };
	key <I252> {	[ 0x100810f4 ] };
	key <I253> {	[ 0x100810f5 ] };
	key <I254> {	[ 0x1008ffb4 ] };
	key <I255> {	[ 0x1008ffb5 ] };
	key <I256> {	[ 0x1008ffb2 ] };
	key <I366> {	[ 0x10081166 ] };
	key <I372> {	[ 0x1008ff30 ] };
	key <I379> {	[ 0x1008ff9c ] };
	key <I380> {	[ 0x1008ffb8 ] };
	key <I382> {	[ 0x1008ffb3 ] };
	key <I383> {	[ 0x10081177 ] };
	key <I397> {	[ 0x10081185 ] };
	key <I400> {	[ 0x10081188 ] };
	key <I401> {	[ 0x1008ff87 ] };
	key <I405> {	[ 0x1008ff20 ] };
	key <I410> {	[ 0x10081192 ] };
	key <I411> {	[ 0x10081193 ] };
	key <I418> {	[ 0x1008ff99 ] };
	key <I419> {	[ 0x1008119b ] };
	key <I424> {	[ 0x100811a0 ] };
	key <I425> {	[ 0x1008ff5e ] };
	key <I426> {	[ 0x1008ff8b ] };
	key <I427> {	[ 0x1008ff8c ] };
	key <I428> {	[ 0x100811a4 ] };
	key <I429> {	[ 0x1008ff89 ] };
	key <I430> {	[ 0x100811a6 ] };
	key <I431> {	[ 0x1008ff5c ] };
	key <I432> {	[ 0x100811a8 ] };
	key <I433> {	[ 0x100811a9 ] };
	key <I434> {	[ 0x100811aa ] };
	key <I435> {	[ 0x1008ff69 ] };
	key <I436> {	[ 0x100811ac ] };
	key <I437> {	[ 0x100811ad ] };
	key <I438> {	[ 0x1008ff8e ] };
	key <I439> {	[ 0x100811af ] };
	key <I440> {	[ 0x100811b0 ] };
	key <I441> {	[ 0x1008ff61 ] };
	key <I442> {	[ 0x24 ] };
	key <I443> {	[ 0x20ac ] };
	key <I444> {	[ 0x1008ff9d ] };
	key <I445> {	[ 0x1008ff9e ] };
	key <I446> {	[ 0x100811b6 ] };
	key <I447> {	[ 0x100811b7 ] };
	key <I448> {	[ 0x100811b8 ] };
	key <I449> {	[ 0x100811b9 ] };
	key <I450> {	[ 0x100811ba ] };
	key <I452> {	[ 0x100811bc ] };
	key <I453> {	[ 0x100811bd ] };
	key <I454> {	[ 0x100811be ] };
	key <I472> {	[ 0x100811d0 ] };
	key <I473> {	[ 0x100811d1 ] };
	key <I493> {	[ 0x100811e5 ] };
	key <I505> {	[ 0xfff1 ] };
	key <I506> {	[ 0xfff2 ] };
	key <I507> {	[ 0xfff3 ] };
	key <I508> {	[ 0xfff4 ] };
	key <I509> {	[ 0xfff5 ] };
	key <I510> {	[ 0xfff6 ] };
	key <I511> {	[ 0xfff7 ] };
	key <I512> {	[ 0xfff8 ] };
	key <I513> {	[ 0xfff9 ] };
	key <I514> {	[ 0xfff1 ] };
	key <I520> {	[ 0x10081200 ] };
	key <I521> {	[ 0x10081201 ] };
	key <I522> {	[ 0x10081202 ] };
	key <I523> {	[ 0x10081203 ] };
	key <I524> {	[ 0x10081204 ] };
	key <I525> {	[ 0x10081205 ] };
	key <I526> {	[ 0x10081206 ] };
	key <I527> {	[ 0x10081207 ] };
	key <I528> {	[ 0x10081208 ] };
	key <I529> {	[ 0x10081209 ] };
	key <I530> {	[ 0x1008120a ] };
	key <I531> {	[ 0x1008120b ] };
	key <I532> {	[ 0x1008120c ] };
	key <I533> {	[ 0x1008120d ] };
	key <I534> {	[ 0x1008120e ] };
	key <I535> {	[ 0x1008120f ] };
	key <I536> {	[ 0x10081210 ] };
	key <I537> {	[ 0x10081211 ] };
	key <I538> {	[ 0x1008ffa9 ] };
	key <I539> {	[ 0x1008ffb0 ] };
	key <I540> {	[ 0x1008ffb1 ] };
	key <I541> {	[ 0x10081215 ] };
	key <I542> {	[ 0x10081216 ] };
	key <I543> {	[ 0x10081217 ] };
	key <I544> {	[ 0x10081218 ] };
	key <I545> {	[ 0x10081219 ] };
	key <I546> {	[ 0x1008121a ] };
	key <I547> {	[ 0x1008121b ] };
	key <I548> {	[ 0x1008121c ] };
	key <I549> {	[ 0x1008121d ] };
	key <I550> {	[ 0x1008121e ] };
	key <I568> {	[ 0x10081230 ] };
	key <I569> {	[ 0x1008ffb7 ] };
	key <I584> {	[ 0x10081240 ] };
	key <I585> {	[ 0x10081241 ] };
	key <I586> {	[ 0x10081242 ] };
	key <I587> {	[ 0x10081243 ] };
	key <I588> {	[ 0x10081244 ] };
	key <I589> {	[ 0x10081245 ] };
	key <I590> {	[ 0x10081246 ] };
	key <I591> {	[ 0x10081247 ] };
	key <I592> {	[ 0xfe08 ] };
	key <I593> {	[ 0x10081249 ] };
	key <I594> {	[ 0x1008124a ] };
	key <I595> {	[ 0x1008124b ] };
	key <I596> {	[ 0x1008124c ] };
	key <I597> {	[ 0x1008124d ] };
	key <I600> {	[ 0x10081250 ] };
	key <I601> {	[ 0x10081251 ] };
	key <I616> {	[ 0x10081260 ] };
	key <I617> {	[ 0x10081261 ] };
	key <I618> {	[ 0x10081262 ] };
	key <I619> {	[ 0x10081263 ] };
	key <I620> {	[ 0x10081264 ] };
	key <I621> {	[ 0x10081265 ] };
	key <I622> {	[ 0x10081266 ] };
	key <I623> {	[ 0x10081267 ] };
	key <I624> {	[ 0x10081268 ] };
	key <I625> {	[ 0x10081269 ] };
	key <I626> {	[ 0x1008126a ] };
	key <I627> {	[ 0x1008126b ] };
	key <I628> {	[ 0x1008126c ] };
	key <I629> {	[ 0x1008126d ] };
	key <I630> {	[ 0x1008126e ] };
	key <I631> {	[ 0x1008126f ] };
	key <I632> {	[ 0x10081270 ] };
	key <I633> {	[ 0x10081271 ] };
	key <I634> {	[ 0x10081272 ] };
	key <I635> {	[ 0x10081273 ] };
	key <I636> {	[ 0x10081274 ] };
	key <I637> {	[ 0x10081275 ] };
	key <I638> {	[ 0x10081276 ] };
	key <I639> {	[ 0x10081277 ] };
	key <I640> {	[ 0x10081278 ] };
	key <I641> {	[ 0x10081279 ] };
	key <I642> {	[ 0x1008127a ] };
	key <I643> {	[ 0x1008127b ] };
	key <I644> {	[ 0x1008127c ] };
	key <I645> {	[ 0x1008127d ] };
	key <I646> {	[ 0x1008127e ] };
	key <I647> {	[ 0x1008127f ] };
	key <I648> {	[ 0x10081280 ] };
	key <I649> {	[ 0x10081281 ] };
	key <I650> {	[ 0x10081282 ] };
	key <I651> {	[ 0x10081283 ] };
	key <I652> {	[ 0x10081284 ] };
	key <I653> {	[ 0x10081285 ] };
	key <I654> {	[ 0x10081286 ] };
	key <I655> {	[ 0x10081287 ] };
	key <I656> {	[ 0x10081288 ] };
	key <I657> {	[ 0x1008ff3b ] };
	key <I664> {	[ 0x10081290 ] };
	key <I665> {	[ 0x10081291 ] };
	key <I666> {	[ 0x10081292 ] };
	key <I667> {	[ 0x10081293 ] };
	key <I668> {	[ 0x10081294 ] };
	key <I669> {	[ 0x10081295 ] };
	key <I670> {	[ 0x10081296 ] };
	key <I671> {	[ 0x10081297 ] };
	key <I672> {	[ 0x10081298 ] };
	key <I673> {	[ 0x10081299 ] };
	key <I674> {	[ 0x1008129a ] };
	key <I675> {	[ 0x1008129b ] };
	key <I676> {	[ 0x1008129c ] };
	key <I677> {	[ 0x1008129d ] };
	key <I678> {	[ 0x1008129e ] };
	key <I679> {	[ 0x1008129f ] };
	key <I680> {	[ 0x100812a0 ] };
	key <I681> {	[ 0x100812a1 ] };
	key <I682> {	[ 0x100812a2 ] };
	key <I683> {	[ 0x100812a3 ] };
	key <I684> {	[ 0x100812a4 ] };
	key <I685> {	[ 0x100812a5 ] };
	key <I686> {	[ 0x100812a6 ] };
	key <I687> {	[ 0x100812a7 ] };
	key <I688> {	[ 0x100812a8 ] };
	key <I689> {	[ 0x100812a9 ] };
	key <I690> {	[ 0x100812aa ] };
	key <I691> {	[ 0x100812ab ] };
	key <I692> {	[ 0x100812ac ] };
	key <I693> {	[ 0x100812ad ] };
	key <I696> {	[ 0x100812b0 ] };
	key <I697> {	[ 0x100812b1 ] };
	key <I698> {	[ 0x100812b2 ] };
	key <I699> {	[ 0x100812b3 ] };
	key <I700> {	[ 0x100812b4 ] };
	key <I701> {	[ 0x100812b5 ] };
	key <I704> {	[ 0x100812b8 ] };
	key <I705> {	[ 0x100812b9 ] };
	key <I706> {	[ 0x100812ba ] };
	key <I707> {	[ 0x100812bb ] };
	key <I708> {	[ 0x100812bc ] };
	modifier_map Shift { <LFSH>, <RTSH> };
	modifier_map Lock { <CAPS> };
	modifier_map Control { <LCTL>, <RCTL> };
	modifier_map Mod1 { <LALT>, <RALT>, <ALT>, <META> };
	modifier_map Mod2 { <NMLK> };
	modifier_map Mod3 { <LVL5> };
	modifier_map Mod4 { <LWIN>, <RWIN>, <SUPR>, <HYPR> };
	modifier_map Mod5 { <LVL3> };
};

};
)xkb";
    return text;
}

}  // namespace xdesk::compositor
