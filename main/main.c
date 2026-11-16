

/* The <...> are for the standard C library
the "..." are for ESP-IDF, FreeRTOS or the USB Host*/
#include <stdio.h>                  // Standard C input/output, such as printf().
#include <string.h>                 // memcpy(), used to copy Xbox startup bytes into the USB transfer buffer.
#include <stdint.h>                 // Fixed-width integer types such as uint8_t and uint32_t.
#include <stdbool.h>                // Boolean values and type used in the code: true, false, and bool.

#include "freertos/FreeRTOS.h"      // FreeRTOS types and constants such as BaseType_t, pdPASS, and portMAX_DELAY.
#include "freertos/task.h"          // Task functions such as xTaskCreate().

#include "esp_err.h"                // ESP-IDF error codes and helpers such as ESP_OK, ESP_ERROR_CHECK(), and esp_err_to_name().
#include "esp_log.h"                // Logging macros such as ESP_LOGI() and ESP_LOG_BUFFER_HEX(), whose output you see in PuTTY.
#include "esp_intr_alloc.h"         // Interrupt flags such as ESP_INTR_FLAG_LEVEL1, used when installing the USB host.
#include "usb/usb_host.h"           // USB Host functions and types for detecting devices, reading descriptors, and transferring data.



// Logging TAG
static const char *TAG = "USB_HOST";

static uint8_t pending_usb_address;             // Address saved by the NEW_DEV callback. 0 means none pending.
static usb_device_handle_t active_xbox;         // Handle of the Xbox being read. NULL means no active capture.
static bool xbox_disconnected;                  // Set to true when the active Xbox is unplugged.






// React to USB devices being detected or disconnected.
static void usb_client_event_cb(const usb_host_client_event_msg_t *event, void *arg) {

    (void)arg;       // This callback leaves its optional argument unused.

    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        pending_usb_address = event->new_dev.address;       // Save the address so usb_client_task() can open the device.
    }
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE && event->dev_gone.dev_hdl == active_xbox) {
        xbox_disconnected = true;       // Tell the packet-reading loop that the controller it was using is gone.
    }
}


// Mark a USB read or write as no longer pending.
static void xbox_transfer_done(usb_transfer_t *transfer) {
    bool *pending = transfer->context;          // Recover the address of input_pending or output_pending.
    *pending = false;                           // Write false to the original flag through that pointer.
}





/////////////////////////////////////// READ XBOX PACKETS ///////////////////////////////////////
static void read_xbox_packets(usb_host_client_handle_t client, usb_device_handle_t device) {
    enum {
        XBOX_IN_EP = 0x82,          // Controller to ESP32
        XBOX_OUT_EP = 0x02,         // ESP32 to controller
        XBOX_PACKET_SIZE = 64
    };

    // GIP startup commands. Each row holds one command.
    static const uint8_t startup[][7] = {
        {0x05, 0x20, 0x00, 0x01, 0x00},                     // Power on (https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/8eaad00e-97e6-4ae0-86fa-471131649d70)
        {0x0A, 0x20, 0x01, 0x03, 0x00, 0x01, 0x14},         // Turn on the LED (https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/ec312389-2e05-4915-85ed-0e8fe9c3d33b)
        {0x06, 0x20, 0x02, 0x02, 0x01, 0x00},               // Authentication complete
    };

    // Claim interface 0, alternate setting 0, to use its endpoints.
    esp_err_t err = usb_host_interface_claim(client, device, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not claim Xbox interface: %s", esp_err_to_name(err));
        return;
    }

    usb_transfer_t *input = NULL;
    usb_transfer_t *output = NULL;
    bool input_pending = false;
    bool output_pending = false;
    size_t command = 0;             // Index of the next startup command to send

    // Track this device so the USB event callback can flag its disconnection.
    xbox_disconnected = false;
    active_xbox = device;


    // Allocate one reusable transfer and buffer for each direction.
    err = usb_host_transfer_alloc(XBOX_PACKET_SIZE, 0, &input);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = usb_host_transfer_alloc(XBOX_PACKET_SIZE, 0, &output);
    if (err != ESP_OK) {
        goto cleanup;
    }


    // Give the completion callback the address of each transfer's pending flag
    // so it can clear that flag when the transfer finishes.
    input->device_handle = device;
    input->bEndpointAddress = XBOX_IN_EP;
    input->num_bytes = XBOX_PACKET_SIZE;
    input->callback = xbox_transfer_done;
    input->context = &input_pending;

    output->device_handle = device;
    output->bEndpointAddress = XBOX_OUT_EP;
    output->callback = xbox_transfer_done;
    output->context = &output_pending;


    // Queue the first read before sending any startup commands.
    err = usb_host_transfer_submit(input);
    input_pending = (err == ESP_OK);


    while (err == ESP_OK && !xbox_disconnected) {

        // Send the next command only when the OUT transfer is available.
        if (!output_pending && command < sizeof(startup) / sizeof(startup[0])) {

            // The header is four bytes. Its fourth byte gives the payload length.
            output->num_bytes = 4 + startup[command][3];
            memcpy(output->data_buffer, startup[command], output->num_bytes);

            err = usb_host_transfer_submit(output);
            if (err != ESP_OK) {
                break;
            }
            output_pending = true;
            ++command;
        }


        // Wait for USB events and run callbacks for completed transfers
        // or device notifications such as disconnection.
        err = usb_host_client_handle_events(client, portMAX_DELAY);
        if (err != ESP_OK || xbox_disconnected) {
            break;
        }


        // Once a submitted command finishes, check whether it succeeded.
        if (command > 0 && !output_pending && output->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGW(TAG, "Xbox startup command %u failed, status %d", (unsigned)command, (int)output->status);
            break;
        }

        if (!input_pending) {
            if (input->status != USB_TRANSFER_STATUS_COMPLETED) {
                ESP_LOGW(TAG, "Xbox In transfer failed, status %d", (int)input->status);
                break;
            }

            // If the first byte identifies a GIP input report, print only
            // the bytes that actually arrived.
            if (input->actual_num_bytes > 0 && input->data_buffer[0] == 0x20) {
                ESP_LOG_BUFFER_HEX("XBOX_IN", input->data_buffer, input->actual_num_bytes);
            }


            // Reuse the same IN transfer to request the next report.
            err = usb_host_transfer_submit(input);
            input_pending = (err == ESP_OK);
        }
    }

    cleanup:
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Xbox capture stopped: %s", esp_err_to_name(err));
    }

    // Halt and flush each endpoint that still has a transfer pending.
    if (input_pending) {
        ESP_ERROR_CHECK(usb_host_endpoint_halt(device, XBOX_IN_EP));
        ESP_ERROR_CHECK(usb_host_endpoint_flush(device, XBOX_IN_EP));
    }

    if (output_pending) {
        ESP_ERROR_CHECK(usb_host_endpoint_halt(device, XBOX_OUT_EP));
        ESP_ERROR_CHECK(usb_host_endpoint_flush(device, XBOX_OUT_EP));
    }


    // Process the callbacks before freeing the transfer objects they use.
    while (input_pending || output_pending) {
        ESP_ERROR_CHECK(usb_host_client_handle_events(client, portMAX_DELAY));
    }

    ESP_ERROR_CHECK(usb_host_transfer_free(output));
    ESP_ERROR_CHECK(usb_host_transfer_free(input));
    ESP_ERROR_CHECK(usb_host_interface_release(client, device, 0));
    active_xbox = NULL;

}
///////////////////////////////////////////////////////////////////////////////////////////////////





/////////////////////////////////////// USB CLIENT TASK //////////////////////////////////////// 
static void usb_client_task(void *arg) {

    // Recover the USB client handle passed in by xTaskCreate().
    usb_host_client_handle_t client = arg;
    ESP_LOGI(TAG, "USB client ready. Plug in the controller.");


    // Constinuously process USB client events
    while (true) {

        // Wait for client events and run the corresponding callbacks.
        // On NEW_DEV, our callback saves the device address.
        ESP_ERROR_CHECK(usb_host_client_handle_events(client, portMAX_DELAY));      // Waits for work for this client. When a "NEW_DEV" notif appears it calls usb_client_event_cb()


        // Copy the saved address, then clear it so we process it only once.
        uint8_t address = pending_usb_address;      // The new_dev.address is localy store in address
        pending_usb_address = 0;                    //clear the saved value

        if (address == 0) {  
            // No new device address was saved. Go back to waiting.
            continue;
        }


        // Open the device at this address. On success, the API writes
        // the device handle into 'device' through &device.
        usb_device_handle_t device;
        esp_err_t err = usb_host_device_open(client, address, &device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not open USB device address %u: %s", (unsigned)address, esp_err_to_name(err));
            continue;
        }


        // Get a pointer to the device descriptor, which holds the VID and PID.
        const usb_device_desc_t *descriptor;
        err = usb_host_get_device_descriptor(device, &descriptor);

        if (err == ESP_OK) {
            // Use -> to read fields through the descriptor pointer.
            ESP_LOGI(TAG, "USB device at address %u: VID=0x%04X PID=0x%04X", 
                    (unsigned)address,
                    (unsigned)descriptor->idVendor,
                    (unsigned)descriptor->idProduct
            );
            
            // Read input packets only when both IDs match our Xbox controller.
            if (descriptor->idVendor == 0x045E && descriptor->idProduct == 0x0B12) {
                read_xbox_packets(client, device);
            }
        }
        else {
            ESP_LOGW(TAG, "Could not read USB descriptor: %s", esp_err_to_name(err));
        }


        /*
        // Optional debugging output for the device's interfaces and endpoints.
        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(device, &config_desc);

        if(err == ESP_OK) {
            ESP_LOGI(TAG, "USB interfaces and endpoints:");
            usb_print_config_descriptor(config_desc, NULL);
        }
        else {
            ESP_LOGW(TAG, "Could not read USB configuration: %s", esp_err_to_name(err));
        }
        */


        // Release this client's device handle before waiting for more events.
        ESP_ERROR_CHECK(usb_host_device_close(client, device));
    }
}
/////////////////////////////////////////////////////////////////////////////////////////////////






//////////////////////////////// USB HOST TASK /////////////////////////////////////
// Started by xTaskCreate in app_main(). Set up USB host mode and service USB bus events.
static void usb_host_task(void *arg) {

    ESP_LOGI(TAG, "Initializing USB Host...");      // ESP_LOGI allows to prints a message with the INFO logging level


    // Configure USB Host
    usb_host_config_t host_config = {               
        .skip_phy_setup = false,                    // Let ESP-IDF configure the built-in USB PHY.
        .intr_flags = ESP_INTR_FLAG_LEVEL1          // Use a level 1 hardware interrupt for USB.
    };


    // Install the host library before registering any clients.
    ESP_ERROR_CHECK(usb_host_install(&host_config));            // "&" give the memory address

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,                                    // Deliver USB client events through a callback.
        .max_num_event_msg = 5,                                     // Allow up to 5 event notifications to wait in the queue.
        .async = {.client_event_callback = usb_client_event_cb}
    };

    // Registration reads client_config and writes the new handle into client.
    usb_host_client_handle_t client;                                        // Declaration of client variable
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client));     // create the register for the client and return ESP_OK or an error code

    

    // Start a separate task to process notifications and read the controller.
    BaseType_t result = xTaskCreate(
        usb_client_task,                // Function the new task runs.
        "usb_client_task",              // Task name for debugging.
        4096,                           // Stack size in bytes.
        client,                         // USB client handle passed to usb_client_task() as arg.
        5,                              // FreeRTOS scheduling priority.
        NULL                            // No FreeRTOS task handle requested.
    );


    // Convert the task creation result to an ESP-IDF status. Abort on failure.
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "USB Host initialized successfully");
    ESP_LOGI(TAG, "USB event task running");




    // Constinuously process USB Host events
    while(true) {
        uint32_t event_flags = 0;
        
        // Wait for and process USB Host Library work, such as enumeration.
        esp_err_t err = usb_host_lib_handle_events(     //keeps the USB Host Library running. It waits for USB work, processes what’s pending, and gives your code two pieces of information.
            portMAX_DELAY,                              // 	Wait indefinitely for work. While waiting, the host task sleeps and FreeRTOS can run other tasks.
            &event_flags                                // Give the function the address of event_flags so it can write any library-wide notifications there.
        );

        // Log errors and return to the event loop.
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB event error: %s", esp_err_to_name(err));
        }
    }
}
//////////////////////////////////////////////////////////////////////////










////////////////////////////////////////////////////////////////////////////
/////////////////////////// Firmware entry point////////////////////////////
////////////////////////////////////////////////////////////////////////////
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 firmware started");

    // Created USB Host FreeRTOS task
    BaseType_t result = xTaskCreate(   // Return pdPASS if success
        usb_host_task,                  // Function the new task runs.
        "usb_host_task",                // Task name for debugging.
        4096,                           // Stack size in bytes.
        NULL,                           // No argument to pass to the task.
        5,                              // FreeRTOS scheduling priority.
        NULL                            // No task handle needed by app_main().
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB Host task");
        return;
    }

    ESP_LOGI(TAG, "USB Host task created");
}
