#include <wiringPi.h>
#include <wiringPiSPI.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
//#include <time.h> // for delay

#define SPI_CHANNEL 0      // SPI Channel (0 or 1)
#define SPI_SPEED 1000000  // 1 MHz

// Define MAX7219 registers
#define TOTAL_REGS 15
#define MAX7219_REG_NOOP 0x00
#define MAX7219_REG_DECODEMODE 0x09
#define MAX7219_REG_INTENSITY 0x0A
#define MAX7219_REG_SCANLIMIT 0x0B
#define MAX7219_REG_SHUTDOWN 0x0C
#define MAX7219_REG_DISPLAYTEST 0x0F

// Define constants for matrix size
#define MATRIX_HEIGHT 8
#define MATRIX_WIDTH 8
#define DISPLAY_X 4
#define DISPLAY_Y 1

#define GRID_WIDTH MATRIX_WIDTH * DISPLAY_X
#define GRID_HEIGHT MATRIX_HEIGHT * DISPLAY_Y

static int displayCount = DISPLAY_X * DISPLAY_Y;

typedef struct {
    uint8_t value;
} Pixel;

typedef struct {
    Pixel pixels[MATRIX_HEIGHT][MATRIX_WIDTH];  // 2D array representing each pixel on the matrix
} MatrixBuffer;

typedef struct {
    MatrixBuffer display[DISPLAY_Y][DISPLAY_X];  // Array of displays
    uint8_t changes[MATRIX_HEIGHT];  // Track changes per register (1 if changed, 0 if not)
} MatrixState;


#pragma pack(push, 1) // Ensure no padding
typedef struct {
    unsigned char signature[2];
    unsigned int fileSize;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t image_data_address;
} BMPFileHeader;

typedef struct {
    uint32_t headerSize;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bitsPerPixel;
    uint32_t compression;
    uint32_t imageSize;
} BMPInfoHeader;
#pragma pack(pop)


// Initialize SPI communication and display settings
void spiSendData(uint8_t *data, int length) {
    wiringPiSPIDataRW(SPI_CHANNEL, data, length);
}

// Add a pixel update to a specific matrix buffer
void setPixel(MatrixState *matrix, int x, int y, uint8_t value) {

    int dispX = x / MATRIX_WIDTH;
    int dispY = y / MATRIX_HEIGHT;
    int localX = x % MATRIX_WIDTH;
    int localY = y % MATRIX_HEIGHT;

    if (matrix->display[dispY][dispX].pixels[localY][localX].value != value) {
        matrix->display[dispY][dispX].pixels[localY][localX].value = value;

        // Mark the register for this row as changed
        matrix->changes[localY] = 1;
    }
}

// Apply updates to all rows (for testing purposes, we will send all rows regardless of changes)
void applyUpdates(MatrixState *matrix) {
    for (int reg = 0; reg < MATRIX_HEIGHT; reg++) {
        if (matrix->changes[reg] != 0) {
            uint8_t buf[2 * displayCount];  // Buffer for SPI communication

            // Initialize buffer with NOOP command
            memset(buf, MAX7219_REG_NOOP, sizeof(buf));

            for (int dispY = 0; dispY < DISPLAY_Y; dispY++) {
                for (int dispX = 0; dispX < DISPLAY_X; dispX++) {
                    int index = (dispY * DISPLAY_X + dispX) * 2;
                    buf[index] = reg + 1;  // MAX7219 registers are 1-based
                    buf[index + 1] = 0;

                    for (int col = 0; col < MATRIX_WIDTH; col++) {
                        int reverse = MATRIX_WIDTH - col - 1;
                        buf[index + 1] |= (matrix->display[dispY][dispX].pixels[reg][reverse].value << col);
                    }
                }
            }

            // Send the updated row via SPI
            spiSendData(buf, 2 * displayCount);
            matrix->changes[reg] = 0;  // Clear change flag for this row
        }
    }
}

// Initialize MAX7219 displays
void initMax7219() {
    uint8_t initCommands[] = {
            MAX7219_REG_SCANLIMIT, 0x07,  // Display all digits
            MAX7219_REG_DECODEMODE, 0x00, // No decode mode
            MAX7219_REG_INTENSITY, 0x01,  // Minimum intensity
            MAX7219_REG_SHUTDOWN, 0x01,   // Exit shutdown mode
    };

    for (int i = 0; i < 7; i += 2) {
        uint8_t buf[2 * displayCount];
        memset(buf, MAX7219_REG_NOOP, sizeof(buf));

        for (int disp = 0; disp < displayCount; disp++) {
            buf[disp * 2] = initCommands[i];
            buf[disp * 2 + 1] = initCommands[i + 1];
        }

        spiSendData(buf, 2 * displayCount);
    }
}

// Clear all pixels on the display
void clearDisplay(MatrixState *matrix) {
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            setPixel(matrix, x, y, 0);
        }
    }
}

// Test the displays
void testDisplays() {
    uint8_t buf[2 * displayCount];

    // Enable test mode
    memset(buf, MAX7219_REG_DISPLAYTEST, sizeof(buf));
    spiSendData(buf, 2 * displayCount);

    delay(1000);

    // Disable test mode and clear the display
    memset(buf, MAX7219_REG_DISPLAYTEST, sizeof(buf));
    for (int i = 0; i < displayCount; i++) buf[i * 2 + 1] = 0x00;
    spiSendData(buf, 2 * displayCount);
}

void readBMP(MatrixState *matrix) {
    BMPFileHeader fileHeader;
    BMPInfoHeader infoHeader;

    printf("Loading bitmap file\n");

    // Read BMP file header
    if (fread(&fileHeader, sizeof(BMPFileHeader), 1, stdin) != 1) {
        fprintf(stderr, "Error reading BMP file header.\n");
        return;
    }

    // Check if BMP signature is correct ("BM")
    if (fileHeader.signature[0] != 'B' || fileHeader.signature[1] != 'M') {
        fprintf(stderr, "Error: Not a valid BMP file (incorrect signature).\n");
        return;
    }

    printf("File size: %u bytes\n", fileHeader.fileSize);
    printf("Data offset: %u bytes\n", fileHeader.image_data_address);

    // Allocate memory for the entire BMP file data
    uint8_t *fileData = (uint8_t *)malloc(fileHeader.fileSize - sizeof(BMPFileHeader));

    if (!fileData) {
        fprintf(stderr, "Memory allocation failed for file data.\n");
        return;
    }

    // Read the rest of the file into memory
    if (fread(fileData, fileHeader.fileSize - sizeof(BMPFileHeader), 1, stdin) != 1) {
        fprintf(stderr, "Error reading BMP file data.\n");
        free(fileData);
        return;
    }

    // Copy BMP info header from file data
    memcpy(&infoHeader, fileData, sizeof(BMPInfoHeader));

    printf("Image dimensions: %d x %d\n", infoHeader.width, infoHeader.height);
    printf("Bits per pixel: %d\n", infoHeader.bitsPerPixel);

    // Validate image size
    if (infoHeader.width != GRID_WIDTH || infoHeader.height != GRID_HEIGHT) {
        fprintf(stderr, "Error: BMP image dimensions (%d x %d) do not match expected grid size (%d x %d)\n",
                infoHeader.width, infoHeader.height, GRID_WIDTH, GRID_HEIGHT);
        free(fileData);
        return;
    }

    // Extract image data
    uint8_t *pixelData = fileData + (fileHeader.image_data_address - sizeof(BMPFileHeader));

    // Calculate row size (padded to a multiple of 4 bytes)
    uint32_t rowSize = ((infoHeader.width + 31) / 32) * 4;  // Each row is padded to the nearest 4-byte boundary

    // BMP stores pixels bottom-up, so we need to reverse the row order
    for (int y = 0; y < GRID_HEIGHT; y++) {
        int bmpRow = GRID_HEIGHT - 1 - y;  // Calculate the actual row in the BMP file

        // Read each byte in the row and interpret each bit
        for (int x = 0; x < GRID_WIDTH; x++) {
            int byteIndex = bmpRow * rowSize + (x / 8);  // Find the byte containing the bit for this pixel
            int bitIndex = 7 - (x % 8);  // Find the specific bit for this pixel (BMP is high bit to low bit)

            // Extract the bit for this pixel
            uint8_t pixelValue = (pixelData[byteIndex] >> bitIndex) & 1;

            // Set the pixel in the matrix (1 = on, 0 = off)
            setPixel(matrix, x, y, pixelValue);
        }
    }

    // Free allocated memory
    free(fileData);
}

int main(int argc, char **argv) {
    // Setup wiringPi and SPI
    if (wiringPiSetup() == -1) {
        printf("wiringPiSetup failed\n");
        return 1;
    }

    if (wiringPiSPISetup(SPI_CHANNEL, SPI_SPEED) < 0) {
        printf("Can't open the SPI bus\n");
        return 1;
    }

    // Initialize displays
    initMax7219();
    printf("Initialized the displays\n");

    // Test displays
    testDisplays();

    // Initialize matrix state
    MatrixState matrix;
    memset(&matrix, 0, sizeof(MatrixState));
    clearDisplay(&matrix);
    applyUpdates(&matrix);



   // readBMP(&matrix);
    //applyUpdates(&matrix);

    printf("Waiting...\n");
    delay(10000);



    // Ensure displays are cleared before exiting
    clearDisplay(&matrix);
    applyUpdates(&matrix);
    printf("Cleared displays before exiting\n");

    return 0;
}
