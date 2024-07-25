/*
  Nice BusT4
  UART communication at 19200 8n1 speed
  Before the data packet, a break is sent with a duration of 519us (10 bits)
  The contents of the packet that could be understood are described in the structure packet_cmd_body_t

  For Oview, 80 is always added to the address.
  Gate controller address unchanged.


Connection

BusT4                       ESP8266

Device Rx Tx GND
9  7  5  3  1  
10 8  6  4  2
place for cable
            1 ---------- Rx
            2 ---------- GND
            4 ---------- Tx
            5 ---------- VCC (24-34V DC)


From the manual nice_dmbm_integration_protocol.pdf

• ADR: This is the address of the NICE network where the devices you want to manage are located. This can be a value from 1 to 63 (1 to 3F).
This value must be in HEX. If the destination is an integration module on DIN-BAR, this value is 0 (adr = 0) if the destination is
is an intelligent engine, this value is 1 (adr = 1).
• EPT: This is the address of the Nice engine included in the network ADR. It can be a value between 1 and 127. This value must be in HEX.
• CMD: This is the command you want to send to the destination (ADR, EPT).
• PRF: profile setup command.
• FNC: This is the function you want to send to the destination (ADR, EPT).
• EVT: This is the event that is sent to the destination (ADR, EPT).
*/

/*
  OVIEW command dumps

  SBS               55 0c 00 ff 00 66 01 05 9D 01 82 01 64 E6 0c
  STOP              55 0c 00 ff 00 66 01 05 9D 01 82 02 64 E5 0c
  OPEN              55 0c 00 ff 00 66 01 05 9D 01 82 03 00 80 0c
  CLOSE             55 0c 00 ff 00 66 01 05 9D 01 82 04 64 E3 0c
  PARENTAL OPEN 1   55 0c 00 ff 00 66 01 05 9D 01 82 05 64 E2 0c
  PARENTAL OPEN 2   55 0c 00 ff 00 66 01 05 9D 01 82 06 64 E1 0c
*/


#pragma once

#include "esphome.h"
#include "esphome/core/component.h"
#include "esphome/core/automation.h"           // to add Action
#include "esphome/components/cover/cover.h"
#include <HardwareSerial.h>
#include "esphome/core/helpers.h"              // parse strings with built-in tools
#include <queue>                               // for working with a queue



namespace esphome {
namespace bus_t4 {

/* for a short call to class members */
using namespace esphome::cover;
//using esp8266::timeoutTemplate::oneShotMs;


static const int _UART_NO=GPIO_NUM_1; /* uart number */
//static const int _UART_NO=UART_NUM_1;
static const int TX_PIN = 17;           /* pin Tx */
static const int RX_PIN = 5;           /* pin Rx */
static const uint32_t BAUD_BREAK = 9200; /* baudrate for a long pulse before the packet */
static const uint32_t BAUD_WORK = 19200; /* working baudrate */
static const uint8_t START_CODE = 0x55; /*packet start byte */

static const float CLOSED_POSITION_THRESHOLD = 0.007;  // The percentage value of the drive position below which the gate is considered fully closed
static const uint32_t POSITION_UPDATE_INTERVAL = 500;  // Update interval of the current drive position, ms

/* esp network settings
          The series can take values from 0 to 63, by default 0
          OVIEW address starts with 8

          When networking several drives with OXI, different rows must be specified for different drives.
          In this case, the OXI must have the same row as the drive it controls.
        */

        /* Packet message type
          so far we are only interested in CMD and INF
          for the rest, I did not check the numbers
          6th byte of CMD and INF packets
        */
enum mes_type : uint8_t {
            CMD = 0x01,  /* number verified, sending commands to automation */
            //  LSC = 0x02,  /* working with script lists */
            //  LST = 0x03,  /* work with automatic lists */
            //  POS = 0x04,  /* request and change the position of automation */
            //  GRP = 0x05,  /* sending commands to a group of automations indicating the bit mask of the motor */
            //  SCN = 0x06,  /* working with scripts */
            //  GRC = 0x07,  /* sending commands to a group of automations created through Nice Screen Configuration Tool */
            INF = 0x08,  /* returns or sets device information */
            //  LGR = 0x09,  /* working with group lists */
            //  CGR = 0x0A,  /* work with categories of groups created through Nice Screen Configuration Tool */
        };




/*
command menu in oview hierarchy
9th byte of CMD packets
*/
enum cmd_mnu  : uint8_t {
  CONTROL = 0x01,
};


/* used in STA responses */
enum sub_run_cmd2 : uint8_t {
  STA_OPENING = 0x02,
  STA_CLOSING = 0x03,
       OPENED = 0x04,
       CLOSED = 0x05,
      ENDTIME = 0x06,  // timeout maneuver completed
      STOPPED = 0x08,
  PART_OPENED = 0x10,  // partial opening
};

/* Errors */
enum errors_byte  : uint8_t {
  NOERR = 0x00, // No error
  FD = 0xFD,    // No command for this device
  };

// Motor types
enum motor_type  : uint8_t {
  SLIDING = 0x01, 
  SECTIONAL = 0x02,
  SWING = 0x03,
  BARRIER = 0x04,
  UPANDOVER = 0x05, // up-and-over подъемно-поворотные ворота
  };

//  9th byte
enum whose_pkt  : uint8_t {
  FOR_ALL = 0x00,  /* package for/from everyone */
  FOR_CU = 0x04,  /* package to/from control unit */
  FOR_OXI = 0x0A,  /* package to/from OXI receiver */
  };
	
// 10th byte of GET/SET of EVT packets, only RUN was encountered for CMD packets
enum setup_submnu : uint8_t {
            WHO = 0x04, // Who is online?
            IN1 = 0x71, // Input setup
            IN2 = 0x72, // Input setup
            IN3 = 0x73, // Input setup
            IN4 = 0x74, // Input setup
            COMM_SBS = 0x61, // Setting up commands - Step by step
            COMM_POPN = 0x62, // Command Settings - Open Partially
            COMM_OPN = 0x63, // Command settings - Open
            COMM_CLS = 0x64, // Command Settings - Close
            COMM_STP = 0x65, // Command setting - STOP
            COMM_PHOTO = 0x68, // Command setup - Photo
            COMM_PHOTO2 = 0x69, // Command settings - Photo2
            COMM_PHOTO3 = 0x6A, // Command settings - Photo3
            COMM_OPN_STP = 0x6B, // Command setting - Stop on opening
            COMM_CLS_STP = 0x6C, // Command settings - Stop on close
            COMM_LET_OPN = 0x78, // Command settings - Interference with opening
            COMM_LET_CLS = 0x79, // Command settings - Interference with closing
            OUT1 = 0x51, // Output settings
            OUT2 = 0x52, // Output settings
            LOCK_TIME = 0x5A, // Output settings - Lock operation time
            S_CUP_TIME = 0x5C, // Output Setting - Suction Cup Time
            LAMP_TIME = 0x5B, // Output settings - courtesy light time
            AUTOCLS = 0x80, // Basic Settings - Auto Close
            P_TIME = 0x81, // Main parameters - Pause time
            PH_CLS_ON = 0x84, // Main Options - Close after Photo - Active
            PH_CLS_VAR = 0x86, // Main Options - Close after Photo - Mode
            PH_CLS_TIME = 0x85, // Basic options - Close after Photo - Waiting time
            ALW_CLS_ON = 0x88, // Basic options - Always close - Active
            ALW_CLS_VAR = 0x8A, // Basic options - Always close - Mode
            ALW_CLS_TIME = 0x89, // Basic options - Always close - Timeout
            OPN_PWR = 0x4A, // Basic parameters - Force control - Opening force
            CLS_PWR = 0x4B, // Basic parameters - Force control - Closing force
            SPEED_OPN = 0x42, // Basic parameters - Speed setting - Opening speed
            SPEED_CLS = 0x43, // Basic parameters - Speed setting - Closing speed
            SPEED_SLW_OPN = 0x45, // Basic parameters - Speed setting - Slow opening speed
            SPEED_SLW_CLS = 0x46, // Basic parameters - Speed setting - Slow closing speed
            START_ON = 0x90, // Basic parameters - Start setting - Active
            START_TIME = 0x91, // Basic parameters - Start setting - Start time
            SLOW_ON = 0xA2, // Main parameters - Slowdown
            BLINK_ON = 0x94, // Basic parameters - Blink - Active
            BLINK_OPN_TIME = 0x95, // Basic parameters - Blink - Opening time
            BLINK_CLS_TIME = 0x99, // Basic parameters - Flicker - Time on closing

            TYPE_M = 0x00, // Actuator type query
            INF_STATUS = 0x01, //	Gate status (Opened/Closed/Stopped)
            MAC = 0x07, // Mac address
            MAN = 0x08, // Manufacturer
            PRD = 0x09, // Product
            HWR = 0x0a, // Hardware version
            FRM = 0x0b, // Firmware version
            DSC = 0x0c, // Description
            CUR_POS = 0x11, // Current position of automation (DPRO924 then waits for positions to be set)
            MAX_OPN = 0x12, // The maximum possible opening according to the encoder.
            POS_MAX = 0x18, // Maximum position (opening) by encoder
            POS_MIN = 0x19, // Minimum position (closing) by encoder
            INF_P_OPN1 = 0x21, //	Partial opening1
            INF_P_OPN2 = 0x22, //	Partial opening2
            INF_P_OPN3 = 0x23, //	Partial opening3
            INF_SLOW_OPN = 0x24, // Slowdown delay in opening
            INF_SLOW_CLS = 0x25, // Slowdown delay in closing
            INF_IO = 0xD1, // Input-output status

            CUR_MAN = 0x02, // Current Maneuver
            SUBMNU = 0x04, // Submenu
            STA = 0xC0, // Status in motion
            MAIN_SET = 0x80, // Main settings
            RUN = 0x82, // Command to execute
        };

	
/* run cmd byte 11 of EVT packets */
        enum run_cmd : uint8_t {
            SET = 0xA9, // parameter change request
            GET = 0x99, // request to get parameters
            GET_SUPP_CMD = 0x89, // get supported commands
        };

        /* The command to be executed.
        11th byte of the CMD packet
        Used in requests and responses */
        enum control_cmd : uint8_t {
            SBS = 0x01, // Step by Step
            STOP = 0x02,   /* Stop */
            OPEN = 0x03,   /* Open */
            CLOSE = 0x04,  /* Close */
            P_OPN1 = 0x05, /* Partial opening 1 */
            P_OPN2 = 0x06, /* Partial opening 2 */
            P_OPN3 = 0x07, /* Partial opening 3 */
            RSP = 0x19, /* interface response acknowledging receipt of the command  */
            EVT = 0x29, /* interface response sending the requested information */

            P_OPN4 = 0x0b, /* Partial opening 4 - shared */
            P_OPN5 = 0x0c, /* Partial opening 5 - Priority step by step */
            P_OPN6 = 0x0d, /* Partial opening 6 - Open and block */
            UNLK_OPN = 0x19, /* Unlock and open */
            CLS_LOCK = 0x0E, /* Close and block */
            UNLCK_CLS = 0x1A, /*  Unlock and close */
            LOCK = 0x0F, /* Lock */
            UNLOCK = 0x10, /* Unlock */
            LIGHT_TIMER = 0x11, /* Light timer */
            LIGHT_SW = 0x12, /* Light on/off */
            HOST_SBS = 0x13, /* Host SBS */
            HOST_OPN = 0x14, /* Lead open */
            HOST_CLS = 0x15, /* Lead close */
            SLAVE_SBS = 0x16, /* Slave SBS */
            SLAVE_OPN = 0x17, /* Slave open */
            SLAVE_CLS = 0x18, /* Slave close */
            AUTO_ON = 0x1B, /* Auto open active */
            AUTO_OFF = 0x1C, /* Auto open inactive */
        };
	
	
/* Информация для лучшего понимания состава пакетов в протоколе */
// тело пакета запроса CMD
// пакеты с размером тела 0x0c=12 байт 
	/*
struct packet_cmd_body_t {
  uint8_t byte_55;              // Заголовок, всегда 0x55
  uint8_t pct_size1;                // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), для команд = 0x0c
  uint8_t for_series;           // серия кому пакет ff = всем
  uint8_t for_address;          // адрес кому пакет ff = всем
  uint8_t from_series;           // серия от кого пакет
  uint8_t from_address;          // адрес от кого пакет
  uint8_t mes_type;           // тип сообщения, 1 = CMD, 8 = INF
  uint8_t mes_size;              // количество байт дальше за вычетом двух байт CRC в конце, для команд = 5
  uint8_t crc1;                // CRC1, XOR шести предыдущих байт
  uint8_t cmd_mnu;                // Меню команды. cmd_mnu = 1 для команд управления
  uint8_t setup_submnu;            // Подменю, в сочетании с группой команды определяет тип отправляемого сообщения
  uint8_t control_cmd;            // Команда, которая должна быть выполнена
  uint8_t offset;            //  Смещение для ответов. Влияет на запросы вроде списка поддерживаемых комманд
  uint8_t crc2;            // crc2, XOR четырех предыдущих байт
  uint8_t pct_size2;            // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), для команд = 0x0c

};





// тело пакета ответа RSP
// пакеты с размером тела 0x0e=14 байт 
struct packet_rsp_body_t {
  uint8_t byte_55;              // Заголовок, всегда 0x55
  uint8_t pct_size1;                // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), >= 0x0e
  uint8_t to_series;           // серия кому пакет ff = всем
  uint8_t to_address;          // адрес кому пакет ff = всем
  uint8_t from_series;           // серия от кого пакет
  uint8_t from_address;          // адрес от кого пакет
  uint8_t mes_type;           // тип сообщения, для этих пакетов всегда  8 = INF
  uint8_t mes_size;              // количество байт дальше за вычетом двух байт CRC в конце, для команд = 5
  uint8_t crc1;                // CRC1, XOR шести предыдущих байт
  uint8_t cmd_mnu;                // Меню команды. cmd_mnu = 1 для команд управления
  uint8_t sub_inf_cmd;            // Из какого подменю получил команду. Значение меньше на 0x80, чем первоначальное подменю
  uint8_t sub_run_cmd;            // Какую команду получил. Значение больше на 0x80, чем полученная команда
  uint8_t hb_data;             // данные, старший бит
  uint8_t lb_data;            // данные, младший бит
  uint8_t err;               // Ошибки
  uint8_t crc2;            // crc2, XOR четырех предыдущих байт
  uint8_t pct_size2;            // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), >= 0x0e

};
	
 // тело пакета ответа с данными EVT
 
 struct packet_evt_body_t {
  uint8_t byte_55;              // Заголовок, всегда 0x55
  uint8_t pct_size1;                // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), >= 0x0e
  uint8_t to_series;           // серия кому пакет ff = всем
  uint8_t to_address;          // адрес кому пакет ff = всем
  uint8_t from_series;           // серия от кого пакет
  uint8_t from_address;          // адрес от кого пакет
  uint8_t mes_type;           // тип сообщения, для этих пакетов всегда  8 = INF
  uint8_t mes_size;              // количество байт дальше за вычетом двух байт CRC в конце, для команд = 5
  uint8_t crc1;                // CRC1, XOR шести предыдущих байт
  uint8_t whose;                // Чей пакет. Варианты: 00 - общий, 04 - контроллера привода, 0A - приемника OXI
  uint8_t setup_submnu;            // Из какого подменю получил команду. Значение равно первоначальному подменю
  uint8_t sub_run_cmd;            // На какую команду отвечаем. Значение меньше на 0x80, чем отправленная ранее команда
  uint8_t next_data;            // Следующий блок данных
  uint8_t err;               // Ошибки
  uint8_t data_blk;            // Блок данных, может занимать несколько байт
  uint8_t crc2;            // crc2, XOR всех предыдущих байт до девятого (Чей пакет)
  uint8_t pct_size2;            // размер тела пакета (без заголовка и CRC. Общее количество  байт минус три), >= 0x0e

};
 
 
*/

enum position_hook_type : uint8_t {
     IGNORE = 0x00,
    STOP_UP = 0x01,
  STOP_DOWN = 0x02
 };

// создаю класс, наследую членов классов Component и Cover
class NiceBusT4 : public Component, public Cover {
  public:
	
    // настройки привода
    bool autocls_flag; // Автозакрывание - L1
    bool photocls_flag; // Закрыть после фото - L2
    bool alwayscls_flag; // Всегда закрывать - L3
    bool init_ok = false; //  определение привода при включении
    bool is_walky = false; // для walky отличается команда запроса положения
    bool is_robus = false; // для robus не нужно переодически запрашивать позицию
		
    void setup() override;
    void loop() override;
    void dump_config() override; // для вывода в лог информации об оборудовнии

    void send_raw_cmd(std::string data);
    void send_cmd(uint8_t data) {this->tx_buffer_.push(gen_control_cmd(data));}	
    void send_inf_cmd(std::string to_addr, std::string whose, std::string command, std::string type_command,  std::string next_data, bool data_on, std::string data_command); // длинная команда
    void set_mcu(std::string command, std::string data_command); // команда контроллеру мотора
		

    void set_class_gate(uint8_t class_gate) { class_gate_ = class_gate; }
    
 /*   void set_update_interval(uint32_t update_interval) {  // интервал получения статуса привода
      this->update_interval_ = update_interval;
    }*/

    cover::CoverTraits get_traits() override;

  protected:
    void control(const cover::CoverCall &call) override;
    void send_command_(const uint8_t *data, uint8_t len);
    void request_position(void);  // Запрос условного текущего положения привода
    void update_position(uint16_t newpos);  // Обновление текущего положения привода

    uint32_t last_position_time{0};  // Время последнего обновления текущего положения
    uint32_t update_interval_{500};
    uint32_t last_update_{0};
    uint32_t last_uart_byte_{0};

    CoverOperation last_published_op;  // Последние опубликованные состояние и положение
    float last_published_pos{-1};

    void publish_state_if_changed(void);

    uint8_t position_hook_type{IGNORE};  // Флаг и позиция установки заданного положения привода
    uint16_t position_hook_value;

    uint8_t class_gate_ = 0x55; // 0x01 sliding, 0x02 sectional, 0x03 swing, 0x04 barrier, 0x05 up-and-over
//    uint8_t last_init_command_;
	
    bool init_cu_flag = false;	
    bool init_oxi_flag = false;	

	
    // переменные для uart
    uint8_t _uart_nr;
    uart_t* _uart = nullptr;
    uint16_t _max_opn = 0;  // максимальная позиция энкодера или таймера
    uint16_t _pos_opn = 2048;  // позиция открытия энкодера или таймера, не для всех приводов.
    uint16_t _pos_cls = 0;  // позиция закрытия энкодера или таймера, не для всех приводов
    uint16_t _pos_usl = 0;  // условная текущая позиция энкодера или таймера, не для всех приводов	
    // настройки заголовка формируемого пакета
    uint8_t addr_from[2] = {0x00, 0x66}; //от кого пакет, адрес bust4 шлюза
    uint8_t addr_to[2]; // = 0x00ff;	 // кому пакет, адрес контроллера привода, которым управляем
    uint8_t addr_oxi[2]; // = 0x000a;	 // адрес приемника

    std::vector<uint8_t> raw_cmd_prepare (std::string data);             // подготовка введенных пользователем данных для возможности отправки	
	
    // генерация inf команд
    std::vector<uint8_t> gen_inf_cmd(const uint8_t to_addr1, const uint8_t to_addr2, const uint8_t whose, const uint8_t inf_cmd, const uint8_t run_cmd, const uint8_t next_data, const std::vector<uint8_t> &data, size_t len);	 // все поля
    std::vector<uint8_t> gen_inf_cmd(const uint8_t whose, const uint8_t inf_cmd, const uint8_t run_cmd) {return gen_inf_cmd(this->addr_to[0], this->addr_to[1], whose, inf_cmd, run_cmd, 0x00, {0x00}, 0 );} // для команд без данных
    std::vector<uint8_t> gen_inf_cmd(const uint8_t whose, const uint8_t inf_cmd, const uint8_t run_cmd, const uint8_t next_data, std::vector<uint8_t> data){
	    return gen_inf_cmd(this->addr_to[0], this->addr_to[1], whose, inf_cmd, run_cmd, next_data, data, data.size());} // для команд c данными
    std::vector<uint8_t> gen_inf_cmd(const uint8_t to_addr1, const uint8_t to_addr2, const uint8_t whose, const uint8_t inf_cmd, const uint8_t run_cmd, const uint8_t next_data){
	    return gen_inf_cmd(to_addr1, to_addr2, whose, inf_cmd, run_cmd, next_data, {0x00}, 0);} // для команд с адресом и без данных 	
    	    
    // генерация cmd команд
    std::vector<uint8_t> gen_control_cmd(const uint8_t control_cmd);	    	
	
    void init_device (const uint8_t addr1, const uint8_t addr2, const uint8_t device );
    void send_array_cmd (std::vector<uint8_t> data);	
    void send_array_cmd (const uint8_t *data, size_t len);


    void parse_status_packet (const std::vector<uint8_t> &data); // разбираем пакет статуса
    
    void handle_char_(uint8_t c);                                         // обработчик полученного байта
    void handle_datapoint_(const uint8_t *buffer, size_t len);          // обработчик полученных данных
    bool validate_message_();                                         // функция проверки полученного сообщения

    std::vector<uint8_t> rx_message_;                          // здесь побайтно накапливается принятое сообщение
    std::queue<std::vector<uint8_t>> tx_buffer_;             // очередь команд для отправки	
    bool ready_to_tx_{true};	                           // флаг возможности отправлять команды
	
    std::vector<uint8_t> manufacturer_ = {0x55, 0x55};  // при инициализации неизвестный производитель
    std::vector<uint8_t> product_;
    std::vector<uint8_t> hardware_;
    std::vector<uint8_t> firmware_;
    std::vector<uint8_t> description_;	
    std::vector<uint8_t> oxi_product;
    std::vector<uint8_t> oxi_hardware;
    std::vector<uint8_t> oxi_firmware;
    std::vector<uint8_t> oxi_description;	

}; //класс

} // namespace bus_t4
} // namespace esphome
