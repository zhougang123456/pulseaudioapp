#include <stdio.h>
typedef struct _PaState PaState;
enum PaType
{
    PA_TYPE_PLAYBACK,
    PA_TYPE_RECORD
};
PaState* pa_state_new(int pa_type);
int pa_state_write(PaState *p, const void* data, int length);
int pa_state_read(PaState *p, void* data, int length);
void pa_state_free(PaState* p);
