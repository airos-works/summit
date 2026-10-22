#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <Message.h>
#include <Picture.h>
#include <PrintJob.h>
#include <View.h>
#include <cstdio>
#include <fstream>
#include <string>

struct PageHeader { int32 pictures; off_t next; int32 reserved[10]; } __attribute__((packed));
int main(int argc,char** argv)
{
    if(argc!=3)return 2;
    BApplication app("application/x-vnd.Summit-PrintSpoolRaster");
    BFile file(argv[1],B_READ_ONLY);print_file_header header;BMessage settings;
    if(file.Read(&header,sizeof(header))!=sizeof(header)||settings.Unflatten(&file)!=B_OK||header.page_count!=3)return 1;
    BRect bounds;if(settings.FindRect("printable_rect",&bounds)!=B_OK)return 1;bounds.OffsetTo(0,0);
    std::printf("SPOOL pages=%d size=%.1fx%.1f\n",header.page_count,bounds.Width(),bounds.Height());
    for(int page=0;page<header.page_count;++page){
        PageHeader entry;if(file.Read(&entry,sizeof(entry))!=sizeof(entry)||entry.pictures!=1)return 1;
        BBitmap bitmap(bounds,B_BITMAP_ACCEPTS_VIEWS,B_RGB32);if(bitmap.InitCheck()!=B_OK)return 1;
        auto* view=new BView(bounds,"spool",B_FOLLOW_NONE,0);bitmap.AddChild(view);if(!bitmap.Lock())return 1;
        view->SetHighColor(255,255,255);view->FillRect(bounds);
        for(int i=0;i<entry.pictures;++i){BPoint point;BRect rect;BPicture picture;
            if(file.Read(&point,sizeof(point))!=sizeof(point)||file.Read(&rect,sizeof(rect))!=sizeof(rect)||picture.Unflatten(&file)!=B_OK)return 1;
            view->DrawPicture(&picture,point);
        }
        view->Sync();bitmap.Unlock();
        int width=bounds.IntegerWidth()+1,height=bounds.IntegerHeight()+1;
        std::ofstream out(std::string(argv[2])+"/spooled-"+std::to_string(page)+".ppm",std::ios::binary);out<<"P6\n"<<width<<' '<<height<<"\n255\n";
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){auto* pixel=static_cast<unsigned char*>(bitmap.Bits())+y*bitmap.BytesPerRow()+x*4;char rgb[]={char(pixel[2]),char(pixel[1]),char(pixel[0])};out.write(rgb,3);}
        if(!out)return 1;std::printf("SPOOL page=%d pictures=%d next=%lld\n",page+1,entry.pictures,static_cast<long long>(entry.next));
        file.Seek(entry.next,SEEK_SET);
    }
}
