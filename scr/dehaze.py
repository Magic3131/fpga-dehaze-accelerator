import cv2
import numpy as np
import matplotlib.pyplot as plt

# 1. 读取有雾图片
img = cv2.imread('foggy.jpg').astype(np.float32) / 255.0
if img is None:
    print("错误：找不到 foggy.jpg")
    exit()

# 2. 求暗通道（用15x15窗口代替3x3，减轻光晕）
kernel = np.ones((15,15), np.uint8)
dark = cv2.erode(np.min(img, axis=2), kernel)

# 3. 估算大气光A（取暗通道最亮的前0.1%）
flat_dark = dark.ravel()
flat_img = img.reshape(-1, 3)
idx = flat_dark.argsort()[-int(0.001*len(flat_dark)):]
A = flat_img[idx].mean(axis=0)

# 4. 计算透射率t（优化版）
omega = 0.85  # 稍微降低去雾强度，保留一点雾的层次感
t = 1 - omega * cv2.erode(np.min(img / A, axis=2), kernel)
#对透射率图做高斯平滑，消除光晕和方块感
t = cv2.GaussianBlur(t, (15, 15), 0)
# 强制 t 不能小于 0.4，强行保护天空和亮部不发黑
t = np.clip(t, 0.4, 1.0) 

# 5. 恢复无雾图像
J = np.zeros_like(img)
for c in range(3):
    J[:,:,c] = (img[:,:,c] - A[c]) / t + A[c]
J = np.clip(J, 0, 1)

# 对结果做 Gamma 校正，让画面重新亮起来、色彩更自然
J = np.power(J, 0.8)

# 6. 显示对比图
plt.figure(figsize=(12, 6))
plt.subplot(1, 2, 1)
plt.title('Original Foggy Image')
plt.imshow(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
plt.axis('off')

plt.subplot(1, 2, 2)
plt.title('Optimized Dehazed Image')
plt.imshow(cv2.cvtColor(J, cv2.COLOR_BGR2RGB))
plt.axis('off')

print("优化版去雾完成！")
plt.show()